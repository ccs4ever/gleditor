# The vagent user guide

vagent is a language for programs that receive events, keep their own knowledge, pursue goals, and
cooperate with other agents. It combines typed messages and belief predicates with goal-directed
expressions: an expression can produce a value, try another possibility, or fail. Each agent runs
one turn at a time. Work that can take time is expressed as a wait between turns.

This guide introduces the language specified by [vagent v3.9](../design/vagent-agent-language.md).
The design document defines the exact contract; this guide teaches how to use it and why its
distinctions matter.

## Contents

- [Who this guide is for](#who-this-guide-is-for)
- [The mental model](#the-mental-model)
- [Your first source file](#your-first-source-file)
- [Source spelling and layout](#source-spelling-and-layout)
- [Values, fields and locals](#values-fields-and-locals)
- [Events and handlers](#events-and-handlers)
- [Conditions test success](#conditions-test-success)
- [Search and expression reference](#search-and-expression-reference)
- [Assignment and rollback boundaries](#assignment-and-rollback-boundaries)
- [Control flow and ordinary methods](#control-flow-and-ordinary-methods)
- [Beliefs are typed facts with sources](#beliefs-are-typed-facts-with-sources)
- [Goals, plans and intentions](#goals-plans-and-intentions)
- [Waiting without blocking](#waiting-without-blocking)
- [Messaging, spaces and identity](#messaging-spaces-and-identity)
- [Capacities, skills and native effects](#capacities-skills-and-native-effects)
- [Behaviors and composition](#behaviors-and-composition)
- [Schedules, clocks and orderly stopping](#schedules-clocks-and-orderly-stopping)
- [Modules and reusable declarations](#modules-and-reusable-declarations)
- [A complete workshop project](#a-complete-workshop-project)
- [Persistence, checkpoints and agent identity](#persistence-checkpoints-and-agent-identity)
- [Metaprogramming and self-modifying agents](#metaprogramming-and-self-modifying-agents)
- [Configuration and operational limits](#configuration-and-operational-limits)
- [Testing agents](#testing-agents)
- [Diagnosing problems](#diagnosing-problems)
- [Keyword reference](#keyword-reference)
- [Initial boundaries and where to continue](#initial-boundaries-and-where-to-continue)
- [Appendix A: Implementation status](#appendix-a-implementation-status)

## Who this guide is for

This guide assumes you have used variables, functions, loops and basic types in another programming
language, and can edit a text file and run a shell command. No prior actor-language,
logic-programming or goal-oriented programming knowledge is needed. Understanding the examples does
not require writing C++.

By working through the guide, you should be able to:

- Write and check a small event-driven agent, then diagnose an intentionally rejected edit.
- Predict success, failure, state rollback and suspension at the statement and turn boundaries.
- Combine beliefs, goals and typed conversations, and state which claims need runtime verification.

For dependencies and build instructions, follow the [README quick start](../README.md#quick-start).
Use the [headless shell setup](testing.md#headless-execution) for the commands below, starting in
the repository or worktree root.

## The mental model

An **agent** is one actor with private state. Another agent cannot follow a pointer into that state,
change its fields, or read its beliefs directly. It sends an event, asks a declared question, or
delegates a declared goal instead.

A **handler** is a reaction to one event. It runs in a turn and may update state or stage actions.
It cannot wait for a device, another agent, or a future event.

A **goal** describes something to pursue. An **intention** is a live instance of that goal. A
**plan** supplies steps for achieving it. An intention can suspend between its steps while the agent
continues receiving events and advancing other intentions.

A **belief** is a typed fact, including the sources that assert it. A field is different: it is
ordinary local state with one stored value. Choose a belief when matching facts and their sources
matters; choose a field for a counter, local phase, option or saved intention id.

A **capacity** declares methods; a **skill** implements them. A native skill connects a capacity to
host code. A method marked `effect` stages an asynchronous action after the turn commits.

A **turn** is one selected input or continuation handled at a time. For the counter below, one Add
input changes the count before the next Add is handled. The agent keeps its state between turns, and
other agents cannot interleave writes into that state.

A **segment** is the part of a plan that runs until it suspends or finishes. Once it suspends, other
turns may change fields and beliefs. A plan local preserves a copied value across that wait; a
handler local lasts only for its one invocation. The later rollback and waiting sections explain
what changes stand and what is undone when work fails.

## Your first source file

This agent keeps a running total. It handles Add events and answers a total question.

**Checkable source; runtime predicted.** Save as `counter.vagent`.

```vagent
module demo:counter

event Add { amount: Int }
question total(): Int

agent Counter(start: Int) {
  var count: Int := start

  on Add as a {
    let next := count + a.amount
    count := next
  }

  answer total(): Int { count }
}
```

### Check Counter

Each `vagent` example is a complete source file. Save it under the caption's filename and check it
separately; names can be reused across examples. For this first file, run:

```sh
build/vagent check counter.vagent
```

Expected output:

```text
counter.vagent: checked
```

Checking accepts the source without running its turns. The examples' runtime descriptions are
predictions; [Appendix A](#appendix-a-implementation-status) records current execution support.

### Run Counter

The planned launcher starts this agent with an initial count of zero using:

```sh
build/vagent run 'demo:counter/Counter(0)'
```

`demo:counter` is the logical module name from the first line, `/Counter` selects the agent, and `0`
supplies its start parameter. Quote the argument so the shell passes the parentheses literally. The
current executable supports `check`; `run` is not available yet, as noted in
[Appendix A](#appendix-a-implementation-status).

With the runtime available, deliver `Add{amount: 3}`, then `Add{amount: 2}` in separate turns. The
count becomes 3, then 5. A peer asking the declared total question receives the count when Counter
handles that question.

`count` is a field that survives from turn to turn during this agent's life; durability across a
process restart requires the checkpoint mechanism described later. `next` is a local in this handler
invocation. `as a` names the event; without it, read `occurrence.amount`. The event's field names
are not imported as bare locals. `answer` is a read-only method that implements the question
signature; it does not subscribe to Add events and is not an ordinary message handler.

Nothing in this example prints the count. A test can inspect committed state or ask the question; a
real cooperating agent can ask it in a plan. The distinction prevents examples from treating an
outgoing message as immediate terminal output.

## Source spelling and layout

Type-like names use `PascalCase`: events, records, goals, agents, behaviors, skills and capacities.
Predicates, questions, fields, locals and methods use `snake_case`. Names beginning with `$` belong
to the runtime; source code cannot declare or assign them. `_` binds nothing.

Write `//` line comments or non-nesting `/* block comments */`. Strings use double quotes, including
escapes such as `\n`, `\t`, `\"`, `\\` and `\u{...}`. Literals include `1_000`, `0x2A`, `3.5`,
`1e-3`, `true`, `false`, `()`, `none`, `250ms`, `2s` and `5min`.

A newline normally separates statements inside a block. A leading continuation such as `&`, `|`,
`++`, a comparison, `.`, `?`, `to`, `by`, `then`, `else` or `do` continues the preceding expression.
A leading `-`, `!`, `*`, `(`, `[` or `{` can start a new expression and does not imply continuation.
Use parentheses when a long expression would otherwise be hard to read. Newlines inside grouping,
lists and record literals are whitespace; nested statement blocks regain significant newlines.
Declarations outside blocks need no statement separators. A semicolon can separate statements on one
line.

The token `<-` is reversible assignment. `a<-1` does not mean “a is less than negative one”; write
`a < -1` for that comparison and add spaces around assignments for clarity.

## Values, fields and locals

The language is statically typed. Parameters, fields, event fields, records, predicates and
questions declare types; locals can infer their type from an initializer.

| Type                          | Use                                                                                                                                                                       |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Int`, `Float`, `Str`, `Bool` | Numeric values, text and Boolean data. Comparisons require matching static types; `1 == 1.0` is rejected.                                                                 |
| `()`                          | Unit: a successful action with no useful value. It is not failure.                                                                                                        |
| `Duration`, `Time`            | A duration such as `2s`, and a turn time or absolute deadline.                                                                                                            |
| `Agent`, `Space`              | Names of living agents or spaces, not pointers to their private state.                                                                                                    |
| `Intention`, `Timer`          | Scalar ids used to refer to work within an agent.                                                                                                                         |
| `Opt<T>`                      | A value that may be absent. Absence is `none`; reading it produces its value or fails.                                                                                    |
| `[T]`                         | A homogeneous list. Values are copied when stored.                                                                                                                        |
| `Map<K, V>`                   | Specified typed key/value collection. Source operations need the future linked prelude/runtime; no implemented map API is advertised here.                                |
| `Cell`                        | A reference to an existing arena cell — the one value not copied on store; `==`/`!=` compare identity. See [metaprogramming](#metaprogramming-and-self-modifying-agents). |
| `Code<T>`                     | A `code"…"` fragment that produces a `T` when run; a bare `Code` accepts any `T`. See [metaprogramming](#metaprogramming-and-self-modifying-agents).                      |
| Declared record/event         | Named fields with static types. An event also has message meaning.                                                                                                        |

Use `var name: Type := value` for a field and `let name := value` for a local. Fields require an
initial value. A field that cannot yet have a value usually starts as an optional initialized to
`none`. A plan's locals are saved when that plan suspends; a handler's locals last only for its
activation in the turn.

Stored lists, maps, records and events copy their values. Assigning the same list to two fields does
not create a shared mutable list. Agent names and intention ids still name the same entity when
copied, because the value is the name/id rather than its arena cells.

### Records: named values

`record Point { x: Int, y: Int }` declares a type. `Point{x: 3, y: 4}` constructs one value of that
type; the field names identify the components. `position.x` reads a component. Here a typed event
carries a Point to an agent:

**Checkable source; runtime predicted.** Save as `coordinates.vagent`.

```vagent
record Point { x: Int, y: Int }
event Move { destination: Point }
agent Coordinates {
  var position: Point := Point{x: 0, y: 0}
  var last_x: Int := 0
  on Move as m {
    position := m.destination
    last_x := position.x
  }
}
```

Predicted trace: one `Move{destination: Point{x: 3, y: 4}}` leaves position at (3, 4), with last_x
holding 3. The Point is copied into the field. A record has no message kind of its own; the
enclosing Move event supplies the delivery meaning. On a copy, change the last assignment to
`last_x := position.y`, check the source, and predict last_x after the same Move before reading on.

**Check your reasoning.** The edit is accepted; the predicted last_x is 4. An Int cannot be
substituted for the Point-valued destination.

## Events and handlers

Declare each event type and named fields. Event patterns use fields by name, so adding or reordering
fields does not silently change positional bindings. An event can extend another event type;
handlers for the subtype and its supertypes can match it.

Every delivered event also exposes these implicit fields, which a declaration cannot redefine:

| Field                | Meaning                                            |
| -------------------- | -------------------------------------------------- |
| `source: Opt<Agent>` | Emitter's stable name; absent for a host delivery. |
| `space: Opt<Space>`  | Space through which it arrived, or absence.        |
| `time: Time`         | The turn's time.                                   |
| `ordinal: Int`       | Emitter's emission counter.                        |

A handler's bracketed guard is a test before the body. It must be **pure**: it cannot change state
or stage actions, including through helpers it calls. Numeric comparisons such as `a.severity > 5`
succeed when they match; the next section explains this success test in detail. Priority sorts
matching handlers within the selected event; it does not move that event ahead of another mailbox
entry or interrupt a running plan segment.

**Checkable source; runtime predicted.** Save as `alarms.vagent`.

```vagent
event Alarm { severity: Int }
agent Alarms {
  var high: Int := 0
  var low: Int := 0
  on Alarm as a [a.severity > 5] priority 2 {
    high := high + 1
    consume
  }
  on Alarm { low := low + 1 }
}
```

Severity 8 runs the first handler and leaves low unchanged. Severity 2 fails that guard and runs the
second handler. Without `consume`, every matching handler runs. The later waiting chapter explains
why `consume` does not hide the event from a matching suspended intention: await matching occurs
before handlers.

Handlers run in merged priority/source/declaration order. Agents can acquire additional handler
ranks through behaviors; there is still one event dispatcher and one sequential actor.

## Conditions test success

An expression produces a value or fails. Some expressions can produce alternatives. A comparison
succeeds with its right operand when it matches and fails otherwise. A condition asks whether an
expression succeeds; it does not convert the expression's value to a Boolean.

That matters for `Bool`: both `true` and `false` are values that successfully evaluate. Write
`enabled == true`, not `if enabled`. The checker rejects a bare Bool-valued condition to catch this
mistake.

**Try.** Suppose an `enabled: Bool` field currently holds `false`. Before looking at the table,
write down which expressions produce a value and which fail. In particular, distinguish the
comparison's result from the result of the surrounding `if`.

______________________________________________________________________

**Check your reasoning.** These are expression traces, not complete source units or CLI output:

| Expression while enabled is false      | Predicted result                                                                         |
| -------------------------------------- | ---------------------------------------------------------------------------------------- |
| `enabled`                              | Succeeds with the value `false`; using this bare Bool as a condition is a checker error. |
| `enabled == true`                      | Fails: it produces no value.                                                             |
| `if enabled == true then ()`           | Skips the body and succeeds with `()`.                                                   |
| `if enabled == true then () else fail` | Runs the failing else arm, so the whole expression fails.                                |

When enabled is true, the comparison succeeds with `true`, and either `if` form runs its body and
succeeds with `()`. The following complete unit uses that distinction:

**Checkable source; runtime predicted.** Save as `switch.vagent`.

```vagent
event Enable
event Work

agent Switch {
  var enabled: Bool := false
  var count: Int := 0
  on Enable { enabled := true }
  on Work {
    if enabled == true then count := count + 1
  }
}
```

An early Work leaves count unchanged, but the handler succeeds. `if` without `else` is total: when
its condition fails, it produces `()`. To make that branch fail its enclosing computation, write an
explicit `else fail`.

### Checkpoint: change one handler

**Try.** Work on a copy of `counter.vagent`. Replace both statements in the Add handler with
`if a.amount > 0 then count := count + a.amount`. Check the edited file, then write down the
predicted field after Add amounts 3 and -2 starting from 0. Explain whether the second handler
fails.

In a copy of Switch, deliberately replace `enabled == true` with `enabled`. Record whether checking
accepts the edit and what its diagnostic means. Restore the comparison and check again. Make your
predictions before reading the feedback below.

______________________________________________________________________

**Check your reasoning.** The Counter edit checks successfully. Its predicted count is 3 after both
inputs: the negative input skips the update, and that handler still succeeds. The Switch edit is
rejected with a diagnostic containing
`TypeMismatch: a Bool condition must compare with '== true' or '== false'`; restoring the comparison
makes the source check successfully again.

Those acceptance/rejection results are observable today. The counter values remain predictions until
an executor exists. Before continuing, identify a field, a handler local, an event binding and a
condition without needing search or goal syntax.

## Search and expression reference

`e1 & e2` continues to `e2` for successful results of `e1`; `e1 | e2` tries alternatives. A bounded
statement takes its first result and discards its remaining choices. These are search operators, not
alternate spellings of `&&` and `||`.

**Checkable source; runtime predicted.** Save as `choices.vagent`.

```vagent
event Check
agent Choices(value: Int) {
  var first: Int := 0
  var accepted: Bool := false
  on Check {
    let n := (1 | 2)
    first := n
    if value == (1 | 2) then accepted := true
  }
}
```

For `Choices(2)`, `first` becomes 1 because the local binding is bounded. The comparison searches
its right operand, so `accepted` becomes true after trying the second alternative. Parentheses are
necessary: `value == 1 | 2` means `(value == 1) | 2`, whose second arm always succeeds.

`not expression` succeeds with unit if the expression fails and fails if it succeeds. It does not
negate a returned Bool value. `!list` generates list elements, `!string` generates characters,
`*value` obtains a size, and `++` concatenates lists or strings.

### Operator precedence

The following is ordered from strongest binding to weakest. Comparisons do not chain; use
`a < b & b < c`. Assignment is right-associative, but its unit result makes ordinary chained
assignment ill-typed.

| Level | Operators                        |
| ----- | -------------------------------- |
| 1     | Call, index, field access        |
| 2     | Unary `-`, `!`, `*`              |
| 3     | `*`, `/`, `%`                    |
| 4     | `+`, `-`                         |
| 5     | `++`                             |
| 6     | `<`, `<=`, `>`, `>=`, `==`, `!=` |
| 7     | `not`                            |
| 8     | `\|`                             |
| 9     | `to` with optional `by`          |
| 10    | `?` for string scanning          |
| 11    | `:=`, `<-`                       |
| 12    | `&`                              |

## Assignment and rollback boundaries

`:=` writes in a bounded position. `<-` offers a write that can be undone when search backtracks
within the statement. Neither makes an external change halfway through a turn: outgoing actions
remain staged until the turn succeeds.

**Checkable source; runtime predicted.** Save as `routes.vagent`.

```vagent
event Choose
agent Routes {
  var budget: Int := 5
  var route: Str := ""
  on Choose {
    let selected := (budget <- 1 & budget > 2 & "first")
                  | (budget <- 3 & "second")
    route := selected
  }
}
```

The first comparison fails, restoring budget to 5 before the second arm runs. That arm succeeds,
leaving budget at 3 and route at `"second"`. Both assignments produce unit; the final string in an
arm supplies the value for `selected`. `:=` is rejected inside those speculative arms.

The word “irreversible” for `:=` is local to expression search, not a promise that a failed handler
or hard-failed turn keeps the write. There are several rollback boundaries:

| Failure                                     | What is undone or retained                                                                               |
| ------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| Backtracking over a speculative branch      | Writes and staged actions under that branch are undone.                                                  |
| A bounded statement fails                   | That statement's writes, including nested work, are undone.                                              |
| A handler fails                             | All writes and staged actions from that handler are undone.                                              |
| An ordinary later statement in a plan fails | Earlier successful statements stand; the current plan fails and another plan may be selected later.      |
| The entire turn fails hard                  | All writes and staged actions from that turn are undone; adapter recovery records the specified outcome. |

For example, an event handler that increments a counter, stages an emission, then executes `fail`
leaves no counter increment or emission. A plan that performs those as separate successful
statements before an ordinary `fail` retains them. Hard fuel/cycle/settle-depth exhaustion is
separate from ordinary statement failure and can still undo the whole turn.

### Follow the enclosing rollback boundary

This unit starts from 5. In its plan, the first successful statement changes budget to 6; the next
statement is an entire nested block:

**Checkable source; runtime predicted.** Save as `nested-rollback.vagent`.

```vagent
event Start
goal ChooseRoute() perform
agent NestedRollback {
  var budget: Int := 5
  on Start { adopt ChooseRoute() }
  plan choose for ChooseRoute {
    budget := 6
    {
      let route := (budget <- 1 & budget > 2 & "first")
                   | (budget <- 3 & "second")
      budget := 9
      fail
    }
  }
}
```

Predict the ordinary-failure path after Start and plan selection, with no hard turn failure:

| Step                                    | Predicted budget | Why                                                                                |
| --------------------------------------- | ---------------- | ---------------------------------------------------------------------------------- |
| First separate statement succeeds       | 6                | Its bounded result stands.                                                         |
| First search arm fails                  | 6                | Backtracking undoes the tentative write of 1.                                      |
| Second arm supplies `"second"`          | 3                | The local's first successful result is retained.                                   |
| `budget := 9` succeeds inside the block | 9                | This still belongs to the enclosing block statement.                               |
| `fail` fails the block and plan         | 6                | The enclosing statement undoes both 3 and 9; its earlier sibling statement stands. |

The final 6 is a prediction, not checker output. If this were a handler body, handler failure would
also undo that earlier sibling write, returning to 5. A hard failure of the whole turn can likewise
undo work that ordinary plan failure preserves.

Use `fail` to stop a handler or plan body unsuccessfully. `return` exits a method or answer; it
cannot return from a plan or handler. `yield` suspends an intention for another turn. These three
keywords are not substitutes for one another.

## Control flow and ordinary methods

`for` iterates deterministically over a bounded list/range expression. Its body does not backtrack
merely to obtain the next element. `while` repeats while a pure condition succeeds. An ordinary body
failure fails the loop; inside a handler that also fails and undoes the handler.

**Checkable source; runtime predicted.** Save as `loops.vagent`.

```vagent
event Run
agent Loops {
  var total: Int := 0
  on Run {
    for n in [1, 2, 3] do total := total + n
    while total < 8 do total := total + 1
  }
}
```

After one Run, total is 8. `1 to 3` would visit the same three integers, including both endpoints. A
bounded `let x := ![1, 2, 3]` instead takes only the first generated element. There is no `every`
loop, `break`, or `next`; express early termination with a `while` condition or split work into
goals.

An ordinary `def` call returns within the same turn. Its last expression gives its value; an
explicit bounded `return` can exit early. Preconditions and postconditions are pure:

**Checkable source; runtime predicted.** Save as `limits.vagent`.

```vagent
event Set { value: Int }
agent Limits {
  var stored: Int := 0
  def capped(value: Int): Int {
    if value > 5 then return 5
    value
  }
  def store(value: Int) requires value >= 0 ensures stored >= 0 {
    stored := value
  }
  on Set as s { store(capped(s.value)) }
}
```

Set with value 9 stores 5. Set with value -2 fails the precondition and leaves the previous field
unchanged. A contract violation is ordinary call/statement failure with cause `contract`, not a
language exception that stops the actor. Since `store` writes a field, it is not callable from a
pure condition or an answer. The purity restriction follows calls transitively.

### String scanning

The binary `text ? expression` establishes a scanning subject and position. This differs from the
`?name` query binding used in a belief term.

**Checkable source; runtime predicted.** Save as `tags.vagent`.

```vagent
import std:goal (tab, match, upto)

event Parse { text: Str }
agent Tags {
  var tag: Str := ""
  on Parse as p {
    let value := p.text ? (tab(match("ID:")) & tab(upto(";")))
    tag := value
  }
}
```

For `Parse{text: "ID:bolt;"}`, this yields `"bolt"`. The prefix match advances past `ID:`;
`tab(upto(";"))` extracts through the position before the semicolon. Missing either delimiter fails
the handler, preserving the previous tag. Executing this example still needs scanner lowering and
runtime helpers; accepting the `std:goal` import supplies neither.

## Beliefs are typed facts with sources

A source records **who asserts a fact**. Imagine A is a child of B and both know the predicate
`ready(job_id: Str)`. The following is a predicted message/state trace, not a sequence of CLI
commands:

| Step                                    | Sources of `ready("j-1")` at B | Does the fact exist?        |
| --------------------------------------- | ------------------------------ | --------------------------- |
| A tells B `ready("j-1")`; B receives it | A                              | Yes.                        |
| B also believes `ready("j-1")`          | A, B                           | Yes: one fact, two sources. |
| A untells that fact; B receives it      | B                              | Yes.                        |
| B forgets its own assertion             | None                           | No.                         |

This is why a fact can survive `forget`: it removes the receiver's own assertion, not another
agent's. A field simply holds one local value; it has no set of asserting sources.

Declare a predicate before using it. An asserted fact must be **ground**: every argument already has
a value, with no `?name` or `?_` query variable. A query uses `?name` to bind unknown arguments. All
argument types are checked against the predicate.

**Checkable source; runtime predicted.** Save as `readings.vagent`.

```vagent
predicate reading(key sensor: Str, value: Int)
event Sample { sensor: Str, value: Int }
event Clear { sensor: Str }
event Summarize

agent Readings {
  var total: Int := 0
  var values: [Int] := []
  on Sample as s { revise reading(s.sensor, s.value) }
  on Clear as c { forget reading(c.sensor, ?old) }
  on Summarize {
    total := 0
    for believes reading(?sensor, ?value) do total := total + value
    values := findall(?v, reading("left", ?v))
  }
}
```

A key identifies which earlier assertion from the same source a new fact replaces. Two Samples for
`"left"`, first 4 then 7, leave this agent's source on `reading("left", 7)`. Other sources can
assert different values for that key; the belief base does not choose a universal winner.

| Form                                     | Effect                                                                                                                                                |
| ---------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `believe reading("left", 7)`             | Add self as a source, replacing self's earlier value for that key.                                                                                    |
| `revise reading("left", 7)`              | Remove self's matching keyed assertions and assert the new value. For an unkeyed predicate, self's earlier assertions of that predicate are replaced. |
| `forget reading("left", ?v)`             | Remove self from matching facts; facts remain while any other source asserts them.                                                                    |
| `believes reading("left", ?v)`           | Query matching facts in clause order, binding `v`. It changes no belief.                                                                              |
| `believes reading("left", ?v) from self` | Query only facts asserted by self.                                                                                                                    |
| `findall(?v, reading("left", ?v))`       | Materialize all matching values as a list.                                                                                                            |

A new fact raises `+reading`; losing its final source raises `-reading`. Adding another source to an
existing fact does not raise a new fact event. A trigger such as `on +reading(?sensor, ?value)`
binds typed variables and has no `as` clause.

`for believes` and `findall` take a logical snapshot of the clause list. Changes made during their
body do not cause newly added clauses to appear in that traversal. A `for believes` loop cannot
suspend. If processing a match needs to wait, first materialize values into a list, or adopt a goal
for each match.

The base is closed-world: an absent fact causes its query to fail. This is useful for membership and
lookup, but a missing percept is not necessarily evidence that the physical world is safe. Seed
beliefs before adopting maintain goals that depend on them.

### Checkpoint: follow a fact's sources

**Try.** In the Readings predicate, sensor is a key. Imagine A and B are children of a fresh
receiver R. Select these deliveries in order, with no Sample event and no self assertion at R:

1. A tells R `reading("left", 4)`.
1. B tells R `reading("left", 4)` too.
1. A tells R `reading("left", 7)`.
1. B untells R `reading("left", 4)`.
1. R runs `forget reading("left", ?v)`.

Predict the facts and sources after each step. Does step 3 replace B's assertion? Does step 5 clear
the remaining fact? Also try a source edit: on a copy of Readings, replace the entire Summarize
handler body with `if believes reading("left", ?v) from self then total := v`, then check the file.
Predict whether that query matches R's final state in this scenario.

______________________________________________________________________

**Check your reasoning.** The edited source is accepted. The table is a language prediction:

| After step | Facts at R and their sources                              |
| ---------- | --------------------------------------------------------- |
| 1          | `reading("left", 4)` from A.                              |
| 2          | One `reading("left", 4)` fact from A and B.               |
| 3          | `reading("left", 4)` from B; `reading("left", 7)` from A. |
| 4          | Only `reading("left", 7)` from A.                         |
| 5          | The same fact from A; R has no own assertion to remove.   |

Keyed replacement affects the sender's source, and self-forget affects R's source. The edited
`from self` query does not match: A is not R, so the conditional skips its assignment. Static
acceptance does not demonstrate these deliveries or query results.

### Sending and withdrawing beliefs

`tell peer fact` stages an assertion whose source at the receiver is the sender. `untell` withdraws
that sender's source. A receiver's `forget` cannot withdraw someone else's assertion. A fact from an
untrusted or unexpected source is still a fact with that source, not automatic permission to act.
Validate source and role in handlers or restrict queries with `from self` as the application needs.

## Goals, plans and intentions

A goal declaration supplies its parameters and commitment kind. An intention holds the parameters,
chosen plan, tried plans, saved step, locals and waiting state for one adoption of that goal.

| Kind       | Meaning                                                                                                                                     |
| ---------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `perform`  | Completing a plan body succeeds.                                                                                                            |
| `achieve`  | The declared `succeed when` must hold; reaching a plan's end without it counts as a failed plan.                                            |
| `maintain` | Stay alive until dropped. When `holds` fails, create a restorer that tries the goal's plans until `holds` is restored or the attempt fails. |

All kinds can include a pure `drop when` clause. At a reconsideration boundary, dropping is checked
before achievement success: when both conditions hold, dropping wins. The initial language uses
single-minded commitment. Blind/open-minded modifiers are deferred.

### Adopt versus wait for a subgoal

**Checkable source; runtime predicted.** Save as `sequence.vagent`.

```vagent
event Start
event Phase { number: Int }
goal Outer() perform
goal Inner() perform

agent Sequence {
  on Start { adopt Outer() }
  plan outer for Outer {
    emit Phase{number: 1}
    achieve Inner()
    emit Phase{number: 3}
  }
  plan inner for Inner { emit Phase{number: 2} }
}
```

`adopt` immediately returns an id and makes a top-level intention ready. `achieve` in a plan creates
a subgoal and suspends its caller until that child ends. The emitted phases are staged in the order
1, 2, 3 across turns, but network arrival order is not guaranteed. The waiting keyword `achieve` can
wait for a perform goal; it is not limited to goals whose declaration kind is `achieve`.

A plan's `where` is a pure applicability condition; bindings it makes are available in its body.
Plan selection takes the first applicable untried named plan in the specified plan list. After a
plan fails, its name is recorded as tried and reselection happens in a later wake turn. With no
applicable untried plan, the goal fails immediately rather than waiting for the world to change. Use
an explicit `await`, a maintain goal, or an intentional re-adoption policy to wait/retry.

Do not make every `GoalFailed` handler re-adopt without limits: a permanently impossible task can
otherwise retry forever. Use the application state, a timer, and an explicit retry budget.

### Saving an intention id

This unit demonstrates optional presence without a magic “no intention” number:

**Checkable source; runtime predicted.** Save as `optionalwork.vagent`.

```vagent
event Start
event Cancel
goal Work() perform

agent OptionalWork {
  var task: Opt<Intention> := none
  on Start { task := adopt Work() }
  on Cancel {
    if task then {
      drop task
      task := none
    }
  }
  plan work for Work { () }
}
```

`if task` tests whether the optional read succeeds with an intention id. `not task` would test
absence. This is not a bare `Bool` condition. A saved id can outlive the intention it once named;
`drop` on an ended id is a successful no-op, and storing an id does not keep an intention alive.

### Achievement and maintenance conditions

This complete unit illustrates an achievement with explicit cancellation:

**Checkable source; runtime predicted.** Save as `preparation.vagent`.

```vagent
predicate ready(item: Str)
predicate abandoned(item: Str)
event Prepare { item: Str }
event Abandon { item: Str }
goal Ready(item: Str) achieve {
  succeed when believes ready(item)
  drop when believes abandoned(item)
}

agent Preparation {
  on Prepare as p { adopt Ready(p.item) }
  on Abandon as a { believe abandoned(a.item) }
  plan prepare for Ready { believe ready(item) }
}
```

The goal succeeds when its fact holds, including when the fact already held before selection. If
abandoned also holds at the relevant boundary, the drop condition wins. Goal conditions run in the
adopting agent's scope and can read its fields and beliefs.

For maintenance, adopt once after the initial data is known:

**Checkable source; runtime predicted.** Save as `watcher.vagent`.

```vagent
predicate level(value: Int)
event Level { value: Int }
goal Enough(minimum: Int) maintain {
  holds believes level(?value) from self & value >= minimum
}

agent Watcher {
  var watch: Opt<Intention> := none
  on Level as l {
    revise level(l.value)
    if not watch then watch := adopt Enough(3)
  }
  plan restore for Enough { revise level(minimum) }
}
```

This is a belief-only simulation. It makes no claim to charge a real battery. The maintain intention
stays alive when a restorer succeeds. If a restorer fails or is dropped, a timed back-off prevents
immediate re-adoption; a new percept does not clear that back-off early. Dropping the maintain
intention itself ends the watch and its restorer.

The runtime raises `PlanFailed`, `GoalAchieved`, `GoalFailed` and `GoalDropped` for the
corresponding transitions. A parent subgoal call receives its child's outcome; dropping only a
subgoal causes the parent's waiting statement to fail with cause `dropped`.

## Waiting without blocking

Plans can suspend on a subgoal, delegated outcome, question, belief, event, effect, or `yield`. A
wait for something outside the agent requires a deadline. Suspension saves intention state as data,
not a C++ stack frame or a live speculative choice point.

### One goal, one event wait

Start with one intention and no belief queries:

**Checkable source; runtime predicted.** Save as `one-wait.vagent`.

```vagent
event Start
event Done
goal WaitOnce() perform
agent OneWait {
  var complete: Bool := false
  on Start { adopt WaitOnce() }
  plan wait_once for WaitOnce {
    await Done as done within 2s
    complete := true
  }
}
```

Use a fresh OneWait for each scenario. This timeline selects turns explicitly; the language does not
promise the displayed wall-clock schedule under load.

| Selected input/work and time   | Predicted state afterward                                      |
| ------------------------------ | -------------------------------------------------------------- |
| Deliver Start at time 0        | A WaitOnce intention is ready; complete is false.              |
| Select its plan at time 0      | Await suspends with a deadline at 2s; complete is still false. |
| Deliver Done at 1s             | The wait captures the event and becomes ready to resume.       |
| Select that continuation at 1s | complete becomes true and the perform goal ends successfully.  |

Alternative: if Done never arrives, select the deadline wake at 2s. The waiting statement and plan
fail, WaitOnce ends failed, and complete stays false. A Done delivered before the await suspends is
not saved for this event wait. You can check the source now; selecting these turns requires the
future harness. Try changing the deadline to `3s`, then predict which rows change before checking
that edit.

### Await a fact or a future event

**Checkable source; runtime predicted.** Save as `waits.vagent`.

```vagent
predicate ready(job_id: Str)
event Ready { job_id: Str }
event Start { job_id: Str }
goal WaitForFact(job_id: Str) perform
goal WaitForEvent(job_id: Str) perform

agent Waits {
  on Ready as r { believe ready(r.job_id) }
  on Start as s {
    adopt WaitForFact(s.job_id)
    adopt WaitForEvent(s.job_id)
  }
  plan wait_fact for WaitForFact { await ready(job_id) within 2s }
  plan wait_event for WaitForEvent {
    await Ready as r [r.job_id == job_id] within 2s
  }
}
```

The belief form tests what is already present before suspending. The event form sees only events
that arrive after its suspension. A Ready delivered before Start can therefore satisfy the fact plan
but not the event plan. Both forms copy the matching payload/bindings when matched, so the plan
later reads that captured value rather than a fact or event cell that may have disappeared.

An elapsed deadline fails the waiting statement. A local recovery `else` can supply a value or
perform bounded recovery, with `cause` bound to the reason. For delegated work or a device effect,
timeout also stages cancellation. A late completion for an ended intention is ignored and counted;
it must not reactivate an old goal.

### Checkpoint: earlier inputs and later waits

**Try.** Use Waits with job id `"j-1"`, no retraction and no timeout before the selected turns.
Predict both plans' behavior in each case:

1. Deliver Ready before Start, then select both plans.
1. Deliver Start, select both plans until they suspend, then deliver Ready.
1. Deliver Start, but deliver Ready before the event plan reaches its await.

For a source edit, replace only the wait_event plan's await with `await ready(job_id) within 2s`.
Check the unit and predict what changes in cases 1 and 3.

______________________________________________________________________

**Check your reasoning.** The edit is accepted. Resumption below means a later selected
continuation; event arrival itself does not synchronously run the rest of the plan.

| Case in the original unit         | Belief wait                                                               | Event wait                                           |
| --------------------------------- | ------------------------------------------------------------------------- | ---------------------------------------------------- |
| Ready before Start                | Finds the stored fact when selected.                                      | Still needs a later matching event after suspension. |
| Both waits suspended before Ready | The handler records the fact, making this wait ready.                     | Captures this event and becomes ready.               |
| Ready before the event await      | Finds the stored fact when selected, or becomes ready if already waiting. | Misses that event; still needs a later one.          |

After the edit, both plans use a belief wait, so the retained fact can satisfy both in cases 1 and
3\. Adopting an intention does not register a future-event wait: execution must first reach the
`await`. These state and timing outcomes remain predictions, not results of `check`.

### Where suspension is legal

A suspension is a plan statement, or a statement inside a permitted `if`, `while` or list/range
`for` body. It is not legal inside an operand of search, comparison, or an ordinary call. Extract a
wait into its own statement, save its value, then call the pure helper. Handlers, ordinary methods,
conditions and answers cannot suspend. `for believes` cannot suspend either.

The `else` in nested constructs attaches to the nearest unmatched `if`, `achieve`, `ask` or `await`.
Brace the intended `then` body when it contains an unhandled wait and the outer `if` also has an
`else`; this makes the recovery boundary visible and avoids ambiguous code.

### Yielding and cancellation

`yield` commits the current segment and makes its intention ready for another turn. It offers other
work a chance to run; it does not wait for a specific message or promise that Cancel will win a
race. `drop id` ends that intention and its subgoals, cancels delegated goals, and stages
best-effort cancellation of an effect in flight. A completed physical effect is not undone by
dropping its plan.

## Messaging, spaces and identity

An agent's stable name survives migration. An `ActorPid` is its current address and can change.
Source programs use names created by spawning; they do not construct agent-name literals or inspect
another agent's private arena.

Every agent hosts `inner`; its parent's inner space is its `outer`. A space holds named members, and
its host relays emissions. A host is also eligible for delivery. The emitter is excluded from its
own emission.

**Checkable source; runtime predicted.** Save as `parent.vagent`.

```vagent
event Ping
event Pong
agent Child {
  on Ping { emit Pong{} in outer for parent }
}
agent Parent {
  var child: Opt<Agent> := none
  var replies: Int := 0
  on Initialize {
    child := spawn Child() in inner
    emit Ping{} in inner for child
  }
  on Pong { replies := replies + 1 }
}
```

The name exists when the spawn is staged; its pid is known after the spawn completes. The local
drain creates the child before its staged Ping is sent. `in` chooses the space and `for` restricts
delivery to a member or host name/list. Without `in`, emission uses outer; without `for`, it fans
out. A refused spawn raises `SpawnFailed` and fails deliveries staged to that name.

The initial society is single-owner. Space membership does not grant stop, spawn, store-write or
delegation authority. Default goal acceptance permits a parent and its children, not arbitrary
members of a space. Stop requests are accepted only from the parent.

Delivery is unordered. Give application conversations their own correlation key and make
application-level responses safe to recognize more than once. Runtime request deduplication helps
transport retry; it is not a durable transaction across process failure or a promise that a device
will act exactly once. A message that exhausts its delivery horizon raises `DeliveryFailed`.

### Questions and answers

Declare each question's argument/result types at top level and implement matching answers in agents
or behaviors. Asking a peer without a matching answer fails with `unanswered`. An answer is
read-only through everything it calls: no field/belief writes, messages, adoption, spawn,
installation or effects. Results still have the inline size bound.

`ask peer.stock(item) within 1s else 0` is a plan statement form. It waits for the receiver's turn,
then yields its typed result or a typed fallback. Choosing 0 as a fallback is an application policy;
it does not prove that the remote inventory is empty. A detailed recovery can inspect `cause`.

### Delegating a goal

`achieve Fetch(order_id) by courier within 5s` sends a delegated goal and waits for its outcome or
refusal. Early cancellation of the caller stages a cancel to the delegate. A receiver that accepts
creates its own intention and eventually sends the outcome; it does not run inside the sender's
turn. The end-to-end project below shows this with declared goals and a spawned child.

First isolate the delegated outcome from application-level belief messages:

**Checkable source; runtime predicted.** Save as `delegation.vagent`.

```vagent
event Start
goal Deliver() perform
agent Courier {
  plan deliver for Deliver { () }
}
agent Dispatch {
  var courier: Opt<Agent> := none
  var finished: Bool := false
  on Initialize { courier := spawn Courier() in inner }
  on Start { adopt Send() }
  goal Send() perform
  plan send for Send {
    achieve Deliver() by courier within 2s
    finished := true
  }
}
```

Predicted sequence: initialize the child, deliver Start, select Send, let the child accept and
finish Deliver, then resume Send on the successful outcome. `finished` becomes true only after that
outcome. This unit sends no `delivered` fact and does not ask a stock question. A failed/refused
outcome fails the wait and leaves finished false; a timeout also stages cancellation.

The workshop adds a separate fact message to this same outcome conversation. They are distinct
messages, so the fact can arrive before or after the outcome. Use a belief await when an already
arrived fact must still satisfy the later wait.

## Capacities, skills and native effects

A capacity is a set of method signatures, not an implementation. `uses` makes those methods
available in the caller's scope. `install` supplies the skill that implements them.

**Checkable source; runtime predicted.** Save as `scaling.vagent`.

```vagent
event Compare
capacity Scale { def adjust(value: Int): Int }
skill Double implements Scale {
  def adjust(value: Int): Int { value * 2 }
}
skill Triple implements Scale {
  def adjust(value: Int): Int { value * 3 }
}
agent Scaling {
  uses Scale
  install Double()
  var first: Int := 0
  var second: Int := 0
  on Compare {
    first := adjust(2)
    install Triple()
    second := adjust(2)
  }
}
```

The fields become 4 and 6. Installing Triple removes the conflicting Double skill first. A skill can
implement several capacities, with one shared field frame; replacement removes the conflicting skill
whole rather than splitting its capacities. Uninstalling does not resurrect an earlier skill. A
skill may use other capacities and has `on install`/`on uninstall` lifecycle blocks.

An effect method is different from these ordinary calls:

**Checkable source; runtime predicted.** Save as `lighting.vagent`.

```vagent
event Start
capacity Lamp {
  effect def set_level(value: Int) within 1s
}
native skill HostLamp implements Lamp
goal Light() perform

agent Lighting {
  uses Lamp
  install HostLamp()
  var finished: Bool := false
  on Start { adopt Light() }
  plan light for Light {
    set_level(3)
    finished := true
  }
}
```

The host must bind the native skill's qualified name to an actual driver. The call stages work, ends
the segment, and resumes only after success/failure/deadline. A quick host action is still
asynchronous in the language. In a handler the same call stages without waiting, and failure later
arrives as `EffectFailed`; it cannot produce its eventual result into a handler local.

Only native skills implement `effect` methods. Every effect declaration needs `within`. A native
binding starts without blocking the node, reports completion as a host delivery, and serializes or
refuses overlapping uses of its device. Its cancel operation is best effort and idempotent.

`effect preempts` is declared on the capacity method. The binding starts it despite another action
of that capacity being in flight, ends the old action with cause `preempted`, and follows the
drain's ordering of cancels, preempting effects, then ordinary effects. Use it for an operation such
as halt only when the host driver implements that contract; the keyword cannot make an arbitrary
device safe or interrupt a running language turn.

Native binding authors must keep logical agent/intention/effect ids, not raw arena references, in
long-lived callbacks. They must not leave host work blocking the owner thread while an agent waits.
Permissions, device access and the capability issuer belong to the host integration, not to a skill
body. Check the adapter's implemented API rather than guessing a registration function from this
guide; that API is an implementation-stage deliverable.

### Recovering from an effect failure

**Checkable source; runtime predicted.** Save as `reader.vagent`.

```vagent
event Read
goal ReadLevel() perform
capacity Sensor {
  effect def read_level(): Int within 1s
}
native skill HostSensor implements Sensor
agent Reader {
  uses Sensor
  install HostSensor()
  var level: Int := -1
  on Read { adopt ReadLevel() }
  plan read for ReadLevel {
    level := read_level() else 0
  }
}
```

With a registered Sensor binding, a successful reply supplies the integer level. A host failure or
elapsed deadline selects 0 here; this fallback is a policy choice, not a measured sensor value. A
detailed recovery may read `cause`. The call and recovery occupy the whole assignment statement, so
the continuation fits in the plan frame. `let value := read_level() else 0` has the same value rule;
a bare effect statement may discard the recovery's result. The fallback does not undo an external
action already performed, and cancellation remains best effort.

Ordinary calls and handlers cannot use effect recovery: handlers do not wait for effects. An
unbraced `if c then level := read_level() else level := 0` keeps its ordinary conditional `else`. To
recover the effect in that branch, write
`if c then { level := read_level() else 0 } else { level := 0 }`. Use a block for this purpose in
loop bodies too. A reversible assignment, nested initializer or other operand cannot hide a wait.

## Behaviors and composition

A behavior contributes fields, handlers, goals and plans to its host. It does not create an actor or
a separate scheduler. Vagent has no agent inheritance; compose behavior instances instead.

**Checkable source; runtime predicted.** Save as `host.vagent`.

```vagent
event Tick
event Remove
behavior Counter {
  var ticks: Int := 0
  on Tick { ticks := ticks + 1 }
}
agent Host {
  behavior Counter
  on Remove { unmount Counter }
}
```

The member form mounts one instance during creation. Its ticks field belongs to that instance. After
Remove, later Tick events no longer run that handler. `mount Counter` is the action form for adding
an instance later. This example uses one instance and relies on no duplicate-instance selection
policy. Handler merging uses agent/behavior source order after priority; plans are named, so
changing mounted contributions does not turn a saved plan name into a different plan.

## Schedules, clocks and orderly stopping

`std:agent` is implicit. Its `Schedules` capacity provides `after`, `periodic` and `cancel`; its
`Clock.now()` reads the one time fixed for the current turn. Code does not read a new clock value at
each expression.

**Checkable source; runtime predicted.** Save as `heartbeat.vagent`.

```vagent
event Tick
event StopTicks
agent Heartbeat(period: Duration) {
  var timer: Opt<Timer> := none
  var count: Int := 0
  on Initialize { timer := Schedules.periodic(period, Tick{}) }
  on Tick { count := count + 1 }
  on StopTicks {
    if timer then {
      Schedules.cancel(timer)
      timer := none
    }
  }
}
```

Timers deliver copied events in wake turns. They are not self-sent mailbox messages; a timer event
can reach its owning agent. Timer ids/deadlines live in state and travel with a checkpoint.
Scheduling has service requirements but is not a hard real-time device guarantee. Handler priority
does not change that.

`terminate` requests this agent's orderly stop after the current turn. A parent uses `stop child` to
request its child's stop. Both orderly paths run `on Destroy` and use the specified reaping path.
`drop` only ends an intention; it does not stop the actor. A host crash or unreported local child
crash is not an orderly language cleanup path. Remote monitored loss can take minutes to establish;
use deadlines for requests rather than assuming prompt failure notification.

Initialization runs before the first dispatch and is not repeated by resume. A failed initialization
is fatal: it cannot safely keep retrying and spawning children forever. `Destroy`, `ChildStopping`
and later runtime loss reports have different purposes; do not process them as proof that a physical
effect has been reversed.

## Modules and reusable declarations

Logical module names use colons, for example `demo:workshop` and `std:goal`. Imports can name a
whole module, use `as` for an alias, or list selected symbols in parentheses. Module spellings are
not filesystem paths, public content identities or agent-name literals. A family-wide spelling
migration is deferred; vagent does not silently rewrite a sibling language's import syntax.

Keep declaration signatures in one place and import their typed events, predicates, questions,
capacities and goals. A capacity method can be qualified when used capacities have the same method
name. Native skill bindings use qualified names too, so host and source must agree on the artifact's
module identity.

The current standalone checker does not provide module publishing, artifact loading or native
binding services. Resolve imports only through the functionality actually implemented by that stage;
loading and validating a sealed Vortex artifact belongs to later compiler/adapter work.

## A complete workshop project

This synthesis unit builds on the earlier delegation example with a stock question and a separately
delivered belief, using no physical device. It is a specification-level communication project: a
dispatcher checks stock, delegates a delivery to its child, then waits for the delivered fact. It
does not reserve real inventory, guarantee physical shipping, or create a user-document update.

**Checkable source; runtime predicted.** Save as `workshop.vagent`.

```vagent
module demo:workshop

predicate delivered(order_id: Str)
question stock(item: Str): Int

event Order { order_id: Str, item: Str, count: Int }
event Completed { order_id: Str }

goal Handle(order_id: Str, item: Str, count: Int) perform
goal Fetch(order_id: Str, item: Str, count: Int) perform

agent Warehouse(units: Int) {
  answer stock(item: Str): Int { units }
}

agent Courier {
  plan fetch for Fetch { tell parent delivered(order_id) }
}

agent Dispatcher(units: Int, deadline: Duration) {
  var warehouse: Opt<Agent> := none
  var courier: Opt<Agent> := none
  var completed: Int := 0

  on Initialize {
    warehouse := spawn Warehouse(units) in inner
    courier := spawn Courier() in inner
  }
  on Order as o { adopt Handle(o.order_id, o.item, o.count) }

  plan handle for Handle {
    let available := ask warehouse.stock(item) within deadline else 0
    if available < count then fail
    achieve Fetch(order_id, item, count) by courier within deadline
    await delivered(order_id) within deadline
    completed := completed + 1
    emit Completed{order_id: order_id}
  }
}
```

Warehouse is a fixed test double: `stock(item)` ignores `item` and always returns its constructor's
`units`. It does not maintain per-item balances or decrement stock after a delivery. A real
inventory service would need those policies; the argument here demonstrates a typed question, not a
keyed inventory implementation.

### Independent workshop extension: report a shortage

**Try before reading the solution.** Save a second copy of the workshop unit. Declare an event named
`Shortage` with an `order_id: Str` field and change the plan so insufficient stock both stages that
event and fails Handle. No Fetch may be delegated and completed must stay unchanged. Use order count
3 in two scenarios: the warehouse answers 2, or the stock question times out and its fallback is 0.
Check the modified unit, then write down the state, staged output and ordinary goal outcome.

Acceptance criteria: both cases stage one Shortage with this order id, leave completed at 0 in a
fresh dispatcher, and fail the current plan without Fetch. With that only plan tried, Handle then
ends failed on its next plan-selection wake. Sufficient stock still follows the original delivery
path. These criteria concern ordinary plan failure; a hard-failed turn can undo its staged output.

Hint: earlier successful plan statements stand when a later statement fails. An emission inside the
same failing nested block is undone with that block. Decide where the two statement boundaries must
be before looking below.

______________________________________________________________________

**Sample solution.** These are edits to the complete workshop unit, not another standalone unit. At
top level, add this event declaration:

```text
event Shortage { order_id: Str }
```

Replace the original `if available < count then fail` statement with two separate statements:

```text
if available < count then emit Shortage{order_id: order_id}
if available < count then fail
```

The first if succeeds and retains its staged Shortage. The second if fails only its own statement;
earlier staged output remains, so no Fetch follows and completed stays 0. PlanFailed records that
attempt. At the next selection wake, no untried plan remains, so Handle ends with GoalFailed.
Putting both actions in `if available < count then { emit Shortage{order_id: order_id}; fail }`
would undo the emission. The same reasoning applies to fallback 0; that value still does not
establish the actual stock. Checking this edited source demonstrates syntax and static acceptance,
not these runtime outcomes.

### Walk through the original workshop

The following trace refers to the unedited base unit above, which has no Shortage declaration or
reporting step.

Once execution is implemented, the scenario constructs Dispatcher with units 40 and deadline 5s,
then delivers one `Order{order_id: "o-1", item: "bolt", count: 3}`:

1. Initialize stages the warehouse and courier spawns. Their names are available to later turns.
1. The Order handler adopts Handle and returns; it does not query or deliver synchronously.
1. Handle asks stock and suspends. Warehouse answers 40 in its own turn; Handle resumes.
1. `available < count` does not match (40 is not less than 3), so the `if` skips its body. Handle
   delegates Fetch and waits.
1. Courier accepts its parent's delegation, tells its parent `delivered("o-1")`, and ends Fetch.
1. The outcome and tell can arrive in either order. After the outcome, the belief await either finds
   the fact already present or waits for it.
1. Handle increments completed and stages Completed. The observer still needs to receive its turn.

Use a fresh order id for each logical order. The snippet intentionally has no duplicate-Order
suppression or stock reservation; a production workflow must specify those policies and keep their
state. The zero fallback treats unavailable stock conservatively, not as a verified stock count. If
delegation or delivery fails, the waiting statement/plan fails and the goal lifecycle reports that
outcome. A delayed delivered fact from another order cannot satisfy this order's wait because the
predicate includes the order id.

For an insufficient-stock case, use a fresh Dispatcher with units 2 and the same order count 3. Now
`available < count` succeeds, its `fail` body fails the current plan, and no Fetch is delegated.
With its only plan tried, Handle ends failed at the next plan-selection wake. The same branch is
chosen if the question times out and its fallback supplies 0. There is no application Shortage event
in this unit: failure is reported through the goal lifecycle.

To practice, trace the successful outcome before the fact and then the fact before the outcome. Both
should reach completed = 1 once the continuation runs. In the latter case, the fact is already
present when the await executes. These are paper traces until a runtime harness exists; a successful
source check cannot tell you that your predicted message schedule occurred.

## Persistence, checkpoints and agent identity

Fields, beliefs and sources, installed skill/behavior state, intentions/locals/waits, children,
spaces, timers and readiness information live in the private arena. Ordinary working-state updates
mint no operations in a user document. The initial language has no StoreWriter operation.

A checkpoint explicitly promotes reachable state into the checkpoint value's own transient store; it
is not navigation history in a visited document. It needs no live marks or choice points, a
nonrunning actor, an empty mailbox, no incomplete sends, no queued host delivery and no running
effect. Ready intentions and future timers alone do not prevent it.

Checkpointing has costs: operations for live cells/links, permascroll bytes for constructed strings,
and reloading the program per agent. It is not a free save of a native call stack. State names plans
and peers by stable names rather than pointers into compiled code. Native callbacks and source
readers need their explicit host lifetime contracts.

Resume restores the agent's stable name but mints a new current pid; peers learn the new address
through movement/update envelopes. Messages already sent to the old address can fail before peers
learn the new one. Restoring ready order and deadlines does not reconstruct an external arrival
schedule. In-flight device work cannot simply move to another node with the agent.

Each user has one append-only permascroll. A bare store directory holds edit decisions and needs the
content it addresses; copying a directory alone is not the general publication or checkpoint
transport procedure. Use the implemented published-value protocol when available, not a guessed
filesystem-copy workflow.

A continuously busy agent may never become quiescent enough for checkpoint/resume reclamation. The
arena's compact operation reclaims dead link entries, not cells and string bytes. Same-pid
busy-agent replacement and shared program bases are deferred extensions. Design long-running
workloads around the supported bounds and measure them; neither extension is supplied by the syntax
checker.

## Metaprogramming and self-modifying agents

Everything so far treats a program as fixed text that runs. vagent can also treat a program as
*data*: a vagent program lives on the same arena as the agent's beliefs, fields and plans, built
from the same cells and links. That makes two things possible. A fragment of source can be carried
as a value, instantiated at compile time or run at run time. And an agent can inspect and change its
own behaviour structure — add a plan, tighten a `where` test, grow a rule rank — with those changes
optionally surviving a checkpoint.

This is the basis of the design's *plasticity* path: mutating behaviour structure, not merely scalar
state. A weight is a scalar cell; a learned plan is structure. The full contract is
[vagent-metaprogramming.md](../design/vagent-metaprogramming.md); this section teaches the usable
surface. As throughout this guide, each fenced unit is checked by `build/vagent check` — which now
runs the complete parse → resolve → **expand** → check pipeline — while the runtime stories remain
predicted behaviour until a `vagent run` command exists.

### Cells and the `cells"…"` path

The meta surface names its targets with `Cell`, the one vagent value that is not copied: a `Cell`
refers to an existing arena cell by identity, so `==`/`!=` compare two handles for the *same* cell
and ordered comparison is a type error. A `cells"path"` expression navigates the arena and yields
`[Cell]` — a list, even for one result, so callers index or `for` down to a single `Cell`.

The path body is VQL path source, embedded behind the `cells` prefix because `#`, `/`, `%`, `?` and
braces collide with vagent's own operators. The supported subset is one anchor followed by signed
dimension steps: `##` is the arena home cell, `$name` names a `Cell`-typed binding followed at run
time, `+d.name`/`-d.name` traverse a dimension edge, and `%` mints a cell. At most one traversal
step is allowed and it must be last. A path containing `%` is an arena write — refused wherever
actions are, like a condition or a pure initializer.

**Checkable source; runtime predicted.** Save as `cells.vagent`.

```vagent
agent Tracker {
  var head: [Cell] := cells"##"

  def members(target: Cell): [Cell] {
    cells"$target"
  }

  on Tick { let found := cells"##/d.items" }
}
event Tick
```

Most metaprogramming code does not spell `cells"…"` itself — `program()`, `agent_cell()`,
`plan_named(…)` and `clauses_of(…)` hand you the cells you need. The path literal matters when you
walk a rank directly, for example `cells"$a/+d.code"` over the agent cell's registered-fragment
rank.

### The `code"…"` literal: a fragment value

A `code"…"` literal is source carried as a value. Like `cells"…"`, the prefix is part of one token —
`code "…"` is an identifier followed by a string and any other prefix is a syntax error. The body is
ordinary vagent expression source, parsed by the same parser, in the scope where the literal
appears.

```text
code"n: Int | n + 1"        a fragment: parameter header `n: Int |`, body `n + 1`
code"#inc n: Int | n + 1"   the same, labelled `inc` for later lookup
code"believes sees(...)"    a test-shaped fragment
code"{ navigate_to(w) }"    a body-shaped fragment, type Code<()>
```

The result has type `Code<T>`: `code"n: Int | n + 1"` is a `Code<Int>` — a fragment that *produces*
an `Int` when it runs; `code"{ … }"` is a `Code<()>` — a fragment that acts. A bare `Code` in a
parameter is a wildcard accepting `Code<T>` for any `T`. Creating the literal is a *pure* value — it
produces a fragment and has no arena effect, whatever the body's own purity. A fragment's free names
bind where the literal is written, which is what makes capture predictable.

### `splice`: compile-time substitution

`splice c` substitutes a fragment's body at compile time — quasiquote's unquote. `splice c(args)`
instantiates a parameterized fragment call-by-value: each argument binds to a fresh local before the
body is substituted. The operand must be statically known at expansion — a literal or a name the
expander can resolve — never a runtime `Cell`.

**Checkable source; runtime predicted.** Save as `fragments.vagent`.

```vagent
event Tick

agent Counter {
  var count: Int := 0

  on Tick {
    let inc := code"n: Int | n + 1"      // Code<Int>, one Int parameter
    let next := splice inc(count)        // expands to { let p0 := count; p0 + 1 }
    count := next
  }
}
```

`splice inc(count)` binds `count` to a fresh `let` local and substitutes the cloned body; the merged
statement lowers once, at compile time. `run(inc, [count])` would do the same instantiation at run
time through a call frame instead of inlining — the difference between quasiquote and eval.

A `def` that splices a `Code` parameter becomes a *template*: the expander specializes one copy per
call site. This is how you write a higher-order construct in a language with no closures.

**Checkable source; runtime predicted.** Save as `template.vagent`.

```vagent
event Tick

agent Mapper {
  var count: Int := 0

  def apply(c: Code<Int>, x: Int): Int { splice c(x) }

  on Tick { count := apply(code"p: Int | p * 2", count) }
}
```

Each call to `apply` expands its own `splice c(x)` with that site's fragment, so `apply` is not a
single compiled body. Three rules bound `splice`: the operand is statically known, the splice
produces a value rather than a place (you cannot `:=` into one), and a fragment that cannot be
resolved is refused at expand time rather than left to fail at run time.

### `static`: compile-time evaluation

`static e` evaluates `e` in the compile arena and substitutes the literal result — a compile-time
constant the language computes rather than you hand-writing it. The operand must be pure, *closed*
(every name bound to a literal or another `static` value — a `var` or a handler local is not) and of
a literal-expressible type: `Int`, `Float`, `Str`, `Bool`, `()`, `Duration`, `Time`, or a list or
record of those. It may not call methods, host functions, `run`, or read cells.

**Checkable source; runtime predicted.** Save as `static_fold.vagent`.

```vagent
event Tick

agent Waiter {
  var started: Int := static (2 * 3)

  def measure(): Int { static (60 * 1000) }

  on Tick {
    let base := static (21)
    let shifted := static (base * 2)     // base is static-bound, so closed
    let folded := static (static (3) + 1)
    started := shifted + folded + static { let a := 2; a * 3 }
  }
}
```

`static (60 * 1000)` folds to `60000` before the artifact is sealed; `static { let a := 2; a * 3 }`
runs a whole block at compile time and substitutes `6`. Writing `static (started * 2)` inside the
handler is refused — `started` is a `var`, bound at run time, so the operand is not closed.

### `import std:meta`: the reflection surface

The mutations and the read surface live behind `import std:meta`; until then `run`, `reify` and the
verbs are unknown names, so a unit that only uses `code"…"`/`splice`/`static` needs no import. Once
imported, the following are available — the reads are `pure`, the rest impure:

| Form                                  | Result            | Meaning                                                            |
| ------------------------------------- | ----------------- | ------------------------------------------------------------------ |
| `program()`                           | `Cell`            | this agent's program-image root                                    |
| `agent_cell()`                        | `Cell`            | this agent's own cell                                              |
| `module_named(name)`                  | `Opt<Cell>`       | a module cell by name                                              |
| `symbol_named(owner, name)`           | `Opt<Cell>`       | a goal/event/symbol cell on the named module                       |
| `clauses_of(cell)`                    | `[Cell]`          | the `d.clause` rank — plans of a goal, handlers of an event type   |
| `plan_named(qualified)`               | `Opt<Cell>`       | a clause cell by its qualified `module:Agent.plan` name            |
| `code_named(label)`                   | `Opt<Code<Cell>>` | a fragment head by its `#label`                                    |
| `test_of(clause)` / `body_of(clause)` | `Opt<Code<Cell>>` | the clause's `where` test / `spin` body wrapped as a `Code` handle |
| `label_of(cell)`                      | `Opt<Str>`        | a head's `#label` or a learned clause's minted `patch:<n>`         |
| `validate(cell)`                      | `Bool`            | well-formedness walk over a fragment head's graph                  |
| `weight_of(cell)`                     | `Opt<Float>`      | the `d.weight`/`d.vars` scalar of a junction or named clause       |
| `value_of(cell)`                      | `any`             | a `Cell`'s bits — the companion to `run` on a `Code<Cell>`         |
| `next_rand(seed)`                     | `Int`             | deterministic pseudo-random mix from a state cell's seed           |
| `run(c)` / `run(c, [args])`           | `T`               | run a fragment in its own frame; `args` a list literal             |
| `reify(c)`                            | `Code<T>`         | mint a fresh closed copy of a fragment                             |

Two honest types recur here. `Opt<…>` results do not auto-unwrap in argument position — read them
with the `if [x] then …` idiom, which both tests presence and hands the value to the body. And
`Code<Cell>` is the type the reflect-wrapped graphs carry (`test_of`, `body_of`, `code_named`):
their produced value's type is not statically known, so `run` on one yields a `Cell` whose bits
`value_of` reads — not a statically-typed `Int`.

**Checkable source; runtime predicted.** Save as `meta_read.vagent`.

```vagent
import std:meta

agent Probe {
  def probe(): Int { run(code"1 + 2") }

  def subtract(): Int { run(code"w: Int, v: Int | w - v", [10, 3]) }

  def root(): Cell { program() }

  def self_cell(): Cell { agent_cell() }
}
```

### `run` and `reify`: executing a fragment at run time

`run(c, [args])` executes a fragment now, in its own frame at the same cursor — same-cursor
execution, not a separate intention. It is *opaque-impure*: the checker cannot see inside the
fragment, so `run` is refused in a `where` test, a pure method or a read-only answer — anywhere a
guaranteed-pure computation is required. `reify(c)` makes a fresh, independently-installable copy of
a fragment, which matters because a grafted head is *consumed* — you cannot graft the same head
twice.

**Checkable source; runtime predicted.** Save as `reify.vagent`.

```vagent
import std:meta

agent Copier {
  def dup(): Int {
    let inc := code"#inc n: Int | n + 1"
    let copy := reify(inc)
    run(copy, [41])
  }

  def lookup(): Cell {
    let named := code_named("inc")
    if [named] then run(named, [1]) else cells"##"[0]
  }
}
```

`run(copy, [41])` returns `42` — the literal's type `Code<Int>` makes the result an `Int`. The
`lookup` method resolves the same fragment by its `#inc` label; `code_named` yields a `Code<Cell>`,
so `run` on it returns a `Cell` rather than an `Int`.

### `graft_*` and `learn_*`: changing the behaviour structure

The mutation verbs come in session/durable pairs. `graft_*` edits the current incarnation only — the
change dies when the agent does or a checkpoint resumes. `learn_*` does the same graft *and* records
a durable patch on the agent's `d.patch` rank, so genesis re-derives the change after a
checkpoint/resume — this is what "learned" means: it persists.

| Verb                                         | Effect                                                                         |
| -------------------------------------------- | ------------------------------------------------------------------------------ |
| `graft_plan(goal, name, test, body)`         | mint a named clause, `test` as `where`, `body` as the plan; append to the goal |
| `graft_handler(event, name, guard, body, p)` | the same on an event type's clause rank, with an integer priority              |
| `graft_where(clause, test)`                  | re-point a clause's `where` test at a new fragment                             |
| `graft_body(clause, body)`                   | re-point a clause's body at a new fragment                                     |
| `graft_method(owner, name, code)`            | install `code` as a callable `owner.name` method, replacing any same-named one |
| `prune(clause)` / `unlearn(cell)`            | unlink a clause from its rank / undo the latest matching patch                 |
| `learn_*` twins                              | each `graft_*` verb plus a durable `d.patch` record                            |
| `set_weight(cell, w)` / `weight_of(cell)`    | write / read a clause- or junction-correlated weight                           |
| `sprout_synapse(from, to, w)`                | mint a `d.synapse` junction naming two endpoints by label, weighted `w`        |

Every one is impure — each writes arena edges. A grafted `test`/`guard` must be `pure`; a `body` is
`Code<()>`. The verbs that take a `Cell` name their target through the arena handle, never a `Code`
value — `prune`/`unlearn`/`graft_where`/`set_weight`/`test_of` all take the clause *cell*.

**Checkable source; runtime predicted.** Save as `tighten.vagent`.

```vagent
import std:meta

event Attack
goal Patrol() perform
predicate threat(level: Float)

agent Guard {
  def patrol() { }

  plan cautious for Patrol where believes threat(?t) & t > 0.7 {
    patrol()
  }

  on Attack as a {
    let c := plan_named("demo:Guard.cautious")
    if [c] then learn_where(c, code"believes threat(?t) & t > 0.9")
  }
}
```

`plan_named` resolves the clause by its qualified `demo:Guard.cautious` name; `learn_where`
validates the new test, re-points the clause's `where` at it and records the patch. Because the
replaced test is program-original, resuming restores the skeleton's own attachment and re-applies
the learned test on top — the change affects the *next* plan selection, since `where` is evaluated
per selection. A `?t` bound inside the test stays inside it: a grafted test cannot export
`?`-bindings to the clause body.

`learn_plan` grows a clause rather than re-pointing one:
`learn_plan("Feed", "near_water", test, body)` appends a fresh clause to `Feed`'s rank, mints its
label `patch:<n>` from a durable serial and records `near_water` as the patch's alias — so `patch:`
is refused as a user `name`, and a learned clause is found by its `patch:<n>` label, not the alias.

### Weights and synapses: the plasticity path

The machinery above is sufficient substrate for weights, synapses and a Hebbian grow-and-strengthen
cycle, using only cells and ranks. A **weight** is a `Float` scalar on a cell — `set_weight` writes
it through the arena handle and `weight_of` reads it back. A **synapse** is a *junction cell* on the
agent's `d.synapse` rank naming two endpoints by qualified-name label, carrying its own weight
scalar — a junction rather than a bare edge because a dimension gives each cell one `+d` neighbour,
so endpoint-to-endpoint links could only express a star. `sprout_synapse(from, to, w)` mints one and
returns its `Cell`.

**Checkable source; runtime predicted.** Save as `hebbian.vagent`.

```vagent
import std:meta

event Sighting
event Meal
goal Feed() perform
predicate sees(what: Str, near: Str)
predicate learned_rule(what: Str)

agent Forager {
  def navigate_to(place: Str) { }

  on Sighting as s {
    // grow a candidate rule the first time we see food near water
    if believes sees("food", "water") & not believes learned_rule("food_water") then {
      learn_plan("Feed", "near_water",
                 code"believes sees(\"food\", \"water\")",
                 code"{ navigate_to(\"water\") }")
      believe learned_rule("food_water")
    }
  }

  on Meal as m {   // it worked — strengthen the whole rank
    let feed := symbol_named("demo", "Feed")
    if [feed] then
      for c in clauses_of(feed) do {
        let w := weight_of(c)
        if [w] then set_weight(c, w + 0.1) else set_weight(c, 0.1)
      }
  }
}
```

This is the design's §9.3 example: a `Sighting` grows a `near_water` clause the first time food is
seen near water, and each `Meal` strengthens every clause on `Feed`'s rank by `0.1`, seeding the
first record at `0.1`. The grown clause participates in plan selection like any plan — it *is* a
plan. One bound: clause tests must be `pure`, so the condition structure can grow and re-point but
cannot itself act — the action lives in the `Code<()>` body.

### What survives a checkpoint; what the checker proves

`learn_*` records are what makes a mutation durable. Graft edits are session-local; a `learn_*`
additionally writes a patch record to the agent's `d.patch` rank, and genesis replays those patches
in ordinal order after a resume — so the learned `near_water` clause, the tightened `where` and the
junction weights all come back without re-running the handler. This is the distinction the
[Persistence](#persistence-checkpoints-and-agent-identity) section's checkpoint story applies to
program structure as well as state: installed copies die with the incarnation, but the *patch
records* that re-derive them are durable.

Checking today verifies everything in these units: the `code"…"` literal parses as vagent source,
`Code<T>` types are checked, `splice`/`static` are expanded in the real pipeline, `std:meta` calls
type-check against their signatures, and the purity/position rules are enforced — `run` refused in a
pure position, `splice` refused on a non-statically-known operand, `static` refused on a non-closed
name, `validate`/`set_weight` refused on a `Code` where a `Cell` is required, and a mutating
`cells"…%"` path refused in a pure position. The runtime behaviour the prose describes — patch
records surviving a resume, learned clauses entering plan selection — is implemented and exercised
by the integration tests, but `vagent` still has no `run` command: as elsewhere, a successful
`check` verifies the source, not that a mutation executed.

## Configuration and operational limits

The launcher/adapter design reads `vagent.*` settings from `system://settings`, with schema and
notes entries. These are not source-level constants or a new YAML configuration format. Registration
and live integration must be implemented before treating the following design defaults as knobs in
an executing vagent binary.

| Setting                      | Design default | What to consider                                                                        |
| ---------------------------- | -------------- | --------------------------------------------------------------------------------------- |
| `vagent.max_actors`          | 128            | Live actor budget after reaping is implemented. Children consume their issuer's budget. |
| `vagent.max_mailbox`         | 128            | Queue capacity; window fan-out instead of flooding it.                                  |
| `vagent.max_deliveries`      | 4096           | Outstanding transport sends, distinct from a receiver's mailbox.                        |
| `vagent.value_operations`    | 65,536         | Checkpoint value operation bound, subject to the actor grant.                           |
| `vagent.belief_event_depth`  | 16             | Settle rounds in one turn; cyclic reactions can exhaust it.                             |
| `vagent.condition_fuel`      | 1,000 steps    | Isolated condition/applicability/await-guard evaluation.                                |
| `vagent.maintain_backoff`    | 30s            | Retry delay after a failed/dropped restorer.                                            |
| `vagent.send_retries`        | 5              | Delivery redeliveries before abandonment.                                               |
| `vagent.send_retry_interval` | 1s             | Spacing of redelivery attempts.                                                         |
| `vagent.renew_fraction`      | 0.8            | Fraction of lease elapsed when renewal starts.                                          |
| `vagent.ready_per_message`   | 4              | Wake ceiling between messages; P4 must also provide finite service.                     |
| `vagent.compact_every`       | 64 turns       | Dead-link-run compaction, not whole-arena garbage collection.                           |

Inline messages and answers have a 32 KiB limit; per-turn constructed string budget is also capped
by the actor grant at at most 32 KiB. Lists and maps are copied when stored, and queries/rank
appends may walk linearly. A thousand intentions or repeated large list copies can therefore cost
more than source size suggests. Keep event payloads small and bound conversation/state growth.

Initial peers share one owner. The host holds the delegated key/capability issuer; an agent cannot
sign its own unlimited child grants. Authenticate transport identities rather than trusting a name
written inside a payload. Delegation acceptance, space membership and device permission are separate
decisions. Cross-owner capabilities and hostless swarm membership are deferred designs.

Do not log credentials, keys or routine document/event payloads. The specified logging category is
`vortex.agent`; use bounded operational information to diagnose failures. A program's possession of
an agent name or a typed message is not authority to write a user's store.

## Testing agents

The specified harness uses a manual clock, in-memory transport, a deterministic capability issuer,
and mock native bindings. It must distinguish one explicit turn from a bounded run-until-idle.
Assertions compare semantic fields, beliefs, intentions, timers and staged outbox rather than
allocation-dependent arena references.

For each example, write down the initial parameters, exact selected input and time, resulting state,
and staged effects/messages. When a host effect is mocked, record its start after commit and inject
completion later; do not have the mock synchronously return through a language call.

A useful test set includes:

- A handler that stages work and then fails: no work is drained.
- A later ordinary plan statement that fails: earlier successful statements remain.
- A hard-failed turn: all that turn's state/outbox is undone and recovery is recorded.
- Initialize once across checkpoint/resume, including children and periodic timers.
- Await capture, timeout, cancellation and a late result for an ended intention.
- Duplicate/reordered delivery, rejected spawn and child-stop/loss handling.
- Arbitration under continuous mailbox traffic with host, timer and ready work also pending.

Transport retry, leases and whole-node scheduling need runtime integration tests as well as
one-arena unit tests. Static `check` validates neither liveness nor a device's cancellation
behavior.

Optional captured replay is deferred. A full per-agent trace must record selected inputs, times,
host results and policy decisions, pin program/runtime/grants/settings, and suppress real effects
while comparing outputs. Inbound messages alone are not lifetime replay, and replaying one agent's
trace does not claim to reconstruct whole-node timing or crash-safe exactly-once effects.

## Diagnosing problems

| Symptom                                          | Check first                                                                                                  |
| ------------------------------------------------ | ------------------------------------------------------------------------------------------------------------ |
| A condition containing a Bool is rejected        | Compare it explicitly with true/false; a Bool value is not a success test.                                   |
| A fallback always succeeds                       | Parenthesize alternation on the right of comparisons.                                                        |
| A state write vanished                           | Identify whether a branch, statement, handler or whole turn failed. `:=` does not bypass outer rollback.     |
| The goal fails without trying to wait            | No applicable untried plan remained. Put an await inside a plan or design a bounded re-adoption policy.      |
| An achievement plan ends but the goal fails      | Its `succeed when` did not hold. A plan's end is sufficient only for perform.                                |
| A maintain restorer repeatedly fails             | Verify seeded self beliefs, real device outcomes and drop/retry policy; belief events do not reset back-off. |
| A belief remains after forget                    | Another source still asserts it. Remove or query the intended source.                                        |
| A query/answer cannot call a helper              | The helper or one of its callees changes state or performs an action. Purity is transitive.                  |
| A nested device call is rejected                 | Extract it into its own plan suspension statement.                                                           |
| An event await did not match an earlier event    | Event-await is future-only, and consume only affects later handlers of the selected event.                   |
| Messages arrive out of order or twice            | Use correlation and idempotent conversation transitions; do not assume wire order.                           |
| An urgent handler runs late                      | Priority is within an event; it cannot interrupt a turn or bypass FIFO.                                      |
| An agent cannot checkpoint                       | Drain its mailbox, sends, host deliveries and effects; do not retain speculative marks.                      |
| A restored agent cannot reach a peer immediately | The peer/address table may still name the old pid; inspect move/update and delivery outcomes.                |
| A declared native skill does nothing             | Verify the actual host binding and driver; a declaration supplies no implementation.                         |

Named failure causes include `contract`, `fuel`, `cycles`, `depth`, `remote_fault`, `refused`,
`dropped`, `migrated`, `busy`, and `preempted` at their specified boundaries. Conditions that
exhaust their isolated budget raise a bounded `ConditionFault` and leave the associated intention
decision unchanged; they do not hard-fail the whole turn. Ordinary language failures differ from an
adapter fault that stops its actor.

## Keyword reference

The table groups words by purpose. Some words are contextual rather than reserved everywhere; field
access and record keys admit words that cannot be used as bare local names. Follow the exact lexical
rules in §8.1 of the design when choosing identifiers.

| Words                                               | Role and common interaction                                                                               |
| --------------------------------------------------- | --------------------------------------------------------------------------------------------------------- |
| `module`, `import`, `as`                            | Logical module declaration/import, aliasing, event or await binding.                                      |
| `record`, `event`, `extends`                        | Typed values/events and event/capacity subtype relationships; see [records](#records-named-values).       |
| `agent`, `behavior`, `mount`, `unmount`             | Actor declaration and reusable in-actor contributions.                                                    |
| `capacity`, `skill`, `native`, `implements`, `uses` | Method contract, implementation, host binding, and caller visibility.                                     |
| `install`, `uninstall`                              | Add/remove a skill; replacing a conflict removes its whole implementation.                                |
| `def`, `return`, `requires`, `ensures`              | Ordinary methods and pure call contracts; return exits a method, not an intention.                        |
| `effect`, `preempts`, `within`                      | Asynchronous native work, explicit preemption contract and deadlines.                                     |
| `var`, `let`, `none`                                | Persistent field, local declaration, optional absence.                                                    |
| `if`, `then`, `else`, `not`                         | Success/failure control flow and local recovery; else associates with the nearest eligible construct.     |
| `for`, `in`, `while`, `do`, `to`, `by`              | Deterministic traversal/loops and inclusive ranges; in/by also occur in message/delegation forms.         |
| `predicate`, `key`, `believe`, `revise`, `forget`   | Typed fact declaration, replacement key and self-source updates.                                          |
| `believes`, `from`, `findall`                       | Pure matching, source restriction and materialized matches.                                               |
| `tell`, `untell`                                    | Staged source-bearing belief messages.                                                                    |
| `goal`, `achieve`, `perform`, `maintain`            | Goal kinds; achieve at a call site also waits for a local/delegated goal.                                 |
| `succeed`, `when`, `holds`, `drop`                  | Pure commitment conditions and explicit cancellation of an intention.                                     |
| `plan`, `where`, `adopt`                            | Goal implementation, pure applicability and nonwaiting top-level adoption.                                |
| `on`, `occurrence`, `priority`, `consume`           | Event handling/binding, within-event order, and stopping later handlers.                                  |
| `emit`, `in`, `for`                                 | Stage an event, choose its space, restrict recipients.                                                    |
| `question`, `answer`, `ask`                         | Typed read-only remote queries and a plan's wait for their result.                                        |
| `await`, `as`, `within`                             | Wait for a fact or future event; capture the match and bound its lifetime.                                |
| `spawn`, `self`, `parent`, `inner`, `outer`         | Named actors, family relationships and space context.                                                     |
| `yield`, `fail`, `stop`, `terminate`                | Cooperate, fail computation, request a child's stop, or stop self.                                        |
| `code`, `splice`, `static`, `cells`                 | Prefix forms — fragment literal, compile-time substitution/eval, arena path — contextual, not bare names. |
| `run`, `reify`, `graft_*`, `learn_*`, `unlearn`     | `std:meta` special forms and mutation verbs — available only after `import std:meta`.                     |

## Initial boundaries and where to continue

The initial language keeps single-owner hosted spaces, bounded inline payloads, single-minded
commitment, fixed plan/ready ordering, restricted suspension and private per-agent programs. It has
no source-level protocol declaration, arbitrary continuation suspension or user-store writing. A
capability to publish selected results, cross-owner admission, large content handles, replay, shared
program bases and busy-agent reclamation each need their deferred contracts and gates.

For exact rules, use [the design](../design/vagent-agent-language.md). Its §12.6 gives more isolated
keyword examples, §11.3 defines the test harness, and §18 records release decisions and deferred
work. Read the first complete runtime implementation and its acceptance tests before treating this
guide's operational scenarios as runnable deployment instructions.

## Appendix A: Implementation status

Implementation is proceeding in iterations. The available user command checks source; it does not
execute agents. You can practice writing accepted units and diagnosing rejected edits today. The
runtime stories in this guide teach the specified language for the future executor.

| Area                                     | Status and what it means for this guide                                                                                                                                                                                                |
| ---------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Syntax, types and static restrictions    | Standalone-source checking is implemented as `build/vagent check FILE...`, running the full parse → resolve → expand → check pipeline. Rejected source produces diagnostics and a nonzero exit; module/template gaps are listed below. |
| Metaprogramming                          | `code"…"`, `Code<T>`, `splice`/`static` and the `std:meta` surface check and expand today; the graft/learn/patch/genesis machinery is implemented in the runtime's integration tests, but no `vagent run` command exercises it yet.    |
| Internal Vortex compiler                 | A scalar-expression lowering kernel passes integration tests, but it does not compile complete agent programs and has no source CLI command.                                                                                           |
| Running an agent                         | `vagent run module/Agent(args)` is the specified future invocation, not an enabled command in the frontend-only iteration.                                                                                                             |
| REPL, event delivery and observing turns | Require complete language lowering and the actor adapter; no interactive command language is invented here.                                                                                                                            |
| Examples                                 | Language examples are complete source units unless marked as prose fragments. Their traces explain specified behavior; frontend acceptance alone does not verify execution.                                                            |
| Native effects and scheduling            | Require the future adapter and explicit host bindings. Declaring a native skill does not supply a device driver.                                                                                                                       |
| Deferred extensions                      | StoreWriter, cross-owner spaces, large-value handles, arbitrary suspension, protocol declarations and shared programs are not initial features.                                                                                        |

Use the implementation's tests and progress record to determine which stage has passed its gates. Do
not infer that a feature executes because its syntax is accepted. In particular, static checking
neither sends messages nor starts agents, devices, timers or a network listener.

The checker currently handles standalone source units. Cross-file imports are refused; standard
module aliases/selectors and global goal/plan templates checked in an adopting agent's scope still
have known checking gaps. Acceptance of those forms is not yet a complete conformance check. See the
[implementation progress record](../design/vagent-implementation.md) for the remaining work and
verified test results.

### Checker command reference

```text
build/vagent --help
build/vagent check FILE...
build/vagent check -
```

`-` reads one standalone unit from standard input. Success prints `FILENAME: checked`; rejected
source reports its location and reason. Exit status 0 means every supplied file was accepted, 1
means a source or read error, and 2 means incorrect command usage. Files are checked independently;
passing several files does not link their modules.
