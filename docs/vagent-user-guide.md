# The vagent user guide

vagent is a language for programs that receive events, keep their own knowledge, pursue goals, and
cooperate with other agents. It combines typed messages and belief predicates with goal-directed
expressions: an expression can produce a value, try another possibility, or fail. Each agent runs
one turn at a time. Work that can take time is expressed as a wait between turns.

This guide introduces the language specified by [vagent v3.9](../design/vagent-agent-language.md).
The design document defines the exact contract; this guide teaches how to use it and why its
distinctions matter.

The imported actor compiler is available through `vagent check-runtime FILE` and
`vagent run FILE --agent TYPE`. The existing `vagent check` command uses a separate bounded
frontend; successful checking there does not establish runtime compatibility. Examples in this guide
retain their stated validation level. See the
[implementation progress](../design/vagent-implementation.md) for this integration boundary.

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
- [Native host modules and sandboxing](#native-host-modules-and-sandboxing)
- [Multi-target cluster packaging](#multi-target-cluster-packaging)
- [Concurrent cluster nodes and peer routing](#concurrent-cluster-nodes-and-peer-routing)
- [Interactive terminal dashboard](#interactive-terminal-dashboard)
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

The launcher starts this agent with an initial count of zero using:

```sh
build/vagent run counter.vagent --agent Counter --arg start=0
```

`counter.vagent` supplies the source file, `--agent Counter` selects the agent type, and
`--arg start=0` initializes its `start` parameter. As noted in
[Appendix A](#appendix-a-implementation-status), `vagent run` compiles the agent into a local
single-process mesh and prints its outer emissions.

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

| Type                          | Use                                                                                                                                        |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| `Int`, `Float`, `Str`, `Bool` | Numeric values, text and Boolean data. Comparisons require matching static types; `1 == 1.0` is rejected.                                  |
| `()`                          | Unit: a successful action with no useful value. It is not failure.                                                                         |
| `Duration`, `Time`            | A duration such as `2s`, and a turn time or absolute deadline.                                                                             |
| `Agent`, `Space`              | Names of living agents or spaces, not pointers to their private state.                                                                     |
| `Intention`, `Timer`          | Scalar ids used to refer to work within an agent.                                                                                          |
| `Opt<T>`                      | A value that may be absent. Absence is `none`; reading it produces its value or fails.                                                     |
| `[T]`                         | A homogeneous list. Values are copied when stored.                                                                                         |
| `Map<K, V>`                   | Specified typed key/value collection. Source operations need the future linked prelude/runtime; no implemented map API is advertised here. |
| Declared record/event         | Named fields with static types. An event also has message meaning.                                                                         |

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

### Upgrading and resuming an agent that arbitrates

An agent whose program has an `arbitrated by` predicate carries source profiles, observation
histories and health records. Resume and the in-place schema upgrade carry all of it through one
routine, `translateEpistemicState`; the reconciler road (`LocalHost::upgrade(pid, CompiledProgram)`)
does not, and refuses populated state with `RouteCarriesNoEpistemicState` naming the other overload.
Every refusal is a typed `TransferRefusal` decided before anything changes. An agent holding an
active intention, a changed or removed policy, a different predicate signature, an unknown shape
version or a state the registry does not own is refused by name.

Quiet the agent first. A busy source never becomes quiescent on its own, so the operator path is:

1. `pauseAdmission(pid, ttl)`, so readings and health actions are refused `AdmissionClosed`.
1. Run the reactor until `quiescence(pid)->quiet()`.
1. `upgrade(...)`.
1. `resumeAdmission(pid)`.

`LocalHost::upgradeQuiescing(pid, program, timeout)` is those four steps. The host must serialise
mesh `receive` and `upgrade`; the executor counts a non-empty mailbox and refuses on it but cannot
close it. A percept posted during the swap is consumed by the new program.

An in-place upgrade copies into an arena whose scratch an upgrade grant caps at 32 KiB, so a profile
graph with many distinct strings can be refused (`PeakCopyBudget` naming the scratch bytes, or a
`Runtime` refusal with `ResourceLimit` when the clauses fill the rest) where a resume of the same
agent succeeds. The refusal leaves the actor as it was.

What the host keeps or loses across a swap (the same rows as `hostState()` in
`epistemic_translate_registry.cpp`):

| State                                          | In-process upgrade                      | Resume                                           |
| ---------------------------------------------- | --------------------------------------- | ------------------------------------------------ |
| Bindings and aliases                           | kept                                    | lost; the host rebinds                           |
| Replay floors                                  | kept                                    | lost; relearned from the first stale advance     |
| Outcome and change logs                        | kept                                    | lost; a resend meets the arena's dedup window    |
| Queued readings, in-flight set                 | zero at quiescence                      | lost                                             |
| Open charges, uncounted, refused-while-faulted | kept                                    | lost                                             |
| Estimator replay marks                         | kept                                    | lost; one replay can count again                 |
| Health grants                                  | kept                                    | lost; operators re-grant                         |
| Reset marks                                    | kept                                    | lost; the arena's `reset_epoch` is authoritative |
| Pending health tickets                         | zero at quiescence                      | refused earlier by `checkpoint`                  |
| Health outcome ring                            | kept                                    | lost with the executor                           |
| Health action sequence (host)                  | kept                                    | restarts; the profile's `action_seq` carries     |
| Latch cache                                    | cleared after the commit                | the new executor's, empty                        |
| `last_receive`, `last_contact`                 | kept                                    | cleared; liveness restarts at first contact      |
| Admission record                               | kept until `resumeAdmission` or its ttl | not applicable                                   |

## Native host modules and sandboxing

While pure vagent programs compile to native Vortex opcode graphs and run within the core mesh actor
runtime, high-performance computing, domain-specific hardware accelerators, and specialized system
integrations require native code execution. The vagent native subsystem
([P20](../design/vagent-native-agent.md)) enables agents whose behavior is backed by compiled shared
libraries (`.so`, `.dylib`, or `.dll`) while strictly preserving the agent safety contract.

To external peers, spaces, and supervisory schedulers, a native agent is indistinguishable from a
pure vagent actor. Its interface, types, and event handlers are constructed through deterministic
native ops that produce an actor program tree identical to one parsed from source.

### Isolation and the two-phase sandbox

Native code never runs inside the node's supervisory process. Instead, the runtime supervisor spawns
a dedicated, unprivileged child process: `vagent-native-host`. This native host is placed under
rigorous OS-level sandboxing before foreign library code executes:

1. **Two-phase OS lockdown:**
   - **Phase 1 (Staging & loading):** The host stages the module binary into an isolated, read-only
     directory (`mode 0500`) owned by the supervisor. Filesystem write capabilities are revoked
     using kernel sandboxing primitives: Linux Landlock, OpenBSD `unveil`, macOS Seatbelt, or
     Windows AppContainer. Dynamic loading (`dlopen` with `RTLD_NOW | RTLD_GLOBAL`) loads
     dependencies in topological order.
   - **Phase 2 (Runtime lockdown):** Immediately following library loading, the host invokes
     irreversible OS lockdowns: Linux seccomp-bpf filters and `PR_SET_MDWE` (Memory Deny Write
     Execute), OpenBSD `pledge("stdio rpath recvfd")`, and Windows ACG (`ProhibitDynamicCode = 1`).
     These prevent the creation of executable memory, memory remapping (`MREMAP_MAYMOVE`), and
     arbitrary system calls.
1. **Dual shared-memory ring buffers:** Communication between the control plane and the native host
   avoids kernel socket overhead. The supervisor configures two lockless ring buffers in shared
   memory:
   - **Ring A (`shm_a`):** Control plane to native host. The shared memory region is mapped
     **read-only** in the native host process (`PROT_READ`).
   - **Ring B (`shm_b`):** Native host to control plane. Mapped read-write
     (`PROT_READ | PROT_WRITE`) for outgoing frames and consumer cursor updates.
   - **Doorbells:** Inter-process wakeups use non-blocking eventfd doorbells on Linux, non-blocking
     pipes on macOS/BSD, or auto-reset Event handles on Windows.
1. **No raw paths or network authority:** The sandboxed process has zero direct network sockets and
   zero file write access. Capabilities such as interactive terminal output must be granted by the
   operator as pre-opened file descriptors via an authority broker, never as ambient filesystem
   paths.

### Sealing native programs: `vagent native seal`

A compiled native shared library is sealed into a `.vnprog` artifact containing the recorded native
ops, module filename, and cryptographically verified digests:

```sh
build/vagent native seal build/greeter.so -o build/greeter.vnprog
```

During sealing, `vagent` executes the module's constructor twice in isolation. It compares the
resulting op logs byte-for-byte; if any non-determinism is detected, the operation aborts with
`NondeterministicBuild`. The generated `.vnprog` binds the op log to the SHA-256 digest of the
shared library, establishing the immutable logical `programRef`:

```text
programRef = SHA-256(L(kProgramFormat) || L("vagent-native/1") || L(op_log) || L(module_digest))
```

### Authority ledgers: approve and withdraw

When a native agent requires external device or host capabilities (such as terminal access), it must
be approved in the deployment's authority ledger (`authorities/`):

```sh
# Grant terminal authority to a native program
build/vagent native approve build/greeter.vnprog \
  --kind terminal --as deployer --ledger authorities/

# Revoke authority when retiring a version
build/vagent native withdraw build/greeter.vnprog \
  --kind terminal --as deployer --ledger authorities/
```

When executing agents with `vagent run`, operator policy is enforced via `--grant-policy`:
`--grant-policy approved` permits execution only when an explicit, unrevoked approval entry exists
in the specified `--ledger`.

## Multi-target cluster packaging

In a multi-node cluster, nodes may run on different operating systems and CPU architectures
(`x86_64-linux-gnu`, `aarch64-linux-gnu`, `aarch64-apple-darwin`, `x86_64-windows-msvc`).
Furthermore, native modules often depend on third-party companion libraries (such as numerical
kernels or specialized drivers) that cannot be assumed to exist in host system paths (`/usr/lib`).

The cluster packaging specification ([P21](../design/vagent-cluster-packaging.md)) introduces the
`.vnpkg` container format. A `.vnpkg` package bundles program definitions, target architecture
slices, companion shared libraries, and cryptographic author signatures into a unified,
content-addressed distribution unit.

### Package kinds and architecture-agnostic `programRef`

A package manifest declares one of three package kinds:

- `pure`: Portable vagent source text or canonical bencoded AST.
- `native`: Native ops with target architecture binary slices.
- `source+native`: Source declarations binding native target implementations.

To support seamless migration across heterogeneous nodes, the logical `programRef` commits to the
program body and a canonical `targets_digest`:

```text
targets_digest = SHA-256(canonical target slices, bundled libraries, and system aliases)
programRef     = SHA-256(L(kProgramFormat) || L("vnpkg-1") || L(kind) || L(program_body) || L(targets_digest))
```

Because `programRef` is uniform across architectures, an agent checkpointed on `x86_64` can migrate
and resume on `aarch64` without altering identity or invalidating authority approvals.

### Bundling shared libraries and security rules

The packager validates every bundled dependency against strict security constraints:

- **SONAME allowlist & no path traversal:** Library basenames must match clean ASCII identifiers
  (`<= 255` bytes). Slashes, backslashes, or directory traversal (`../../`) fail with
  `InvalidSonamePath`.
- **System library shadowing forbidden:** Packages cannot bundle host system libraries such as
  `libc.so.6`, `libm.so.6`, or `ld-linux-*.so.*`. Collisions fail with
  `SystemLibraryShadowingForbidden`.
- **Topological dependency resolution:** The packager inspects dynamic ELF tags (`DT_NEEDED`,
  `DT_SONAME`) and sorts bundled libraries in topological load order for `dlopen`.
- **System library aliases:** When identical interfaces have platform-varying names, aliases map
  canonical sonames to host equivalents (e.g.,
  `--alias x86_64-linux-gnu=libncurses.so.6:libncursesw.so.6`).
- **Cryptographic signing:** Author provenance is signed using Ed25519 keys
  (`--author <name> --key <path>`). Private keys must have strict file permissions `0600`.

### Building and inspecting packages

Build a multi-target cluster package with `vagent package`:

```sh
# Package a pure source program
build/vagent package app.vagent -o app.vnpkg

# Package a multi-target agent with bundled companion libraries
build/vagent package app.vagent \
  --target x86_64-linux-gnu=build/x86_64/skills.so \
  --target aarch64-linux-gnu=build/aarch64/skills.so \
  --bundle-lib x86_64-linux-gnu=build/x86_64/libengine.so.1 \
  --bundle-dir aarch64-linux-gnu=build/aarch64/libs \
  --author "alice" --key ~/.vagent/keys/alice.key \
  -o app.vnpkg
```

Inspect a package's contents, dependencies, and cryptographic digests with `vagent package inspect`:

```sh
build/vagent package inspect app.vnpkg
```

Sample output:

```text
Package Format:    vagent-package-1
Kind:              source+native
Logical Ref:       sha256:d8a4e1f7c2b09a...
Author:            alice (ed25519:3b2a104c...)
Signature:         Valid (verified against author public key)

Target: x86_64-linux-gnu
  Module:          skills.so (size: 245,760, sha256: 4a5b6c...)
  Bundled Libs (topological load order):
    1. libengine.so.1 (canonical: libengine.so.1, size: 524,288, sha256: 1c2b3a...)
  System Library Aliases:
    - none

Target: aarch64-linux-gnu
  Module:          skills.so (size: 258,048, sha256: 7d8e9f...)
  Bundled Libs (topological load order):
    1. libengine.so.1 (canonical: libengine.so.1, size: 536,576, sha256: 5e6f7a...)
  System Library Aliases:
    - none
```

### Swarm distribution and Content-Addressed Storage (CAS)

Cluster nodes store received blobs in a local Content-Addressed Store (`CAS`). Packages under 1 MiB
are pushed inline over QUIC stream Lane 1 as `PackageBundle` records. Larger packages stream
on-demand across cluster peers using BitTorrent piece verification and 1 KiB Merkle leaf validation,
ensuring tamper-evident delivery before staging.

## Concurrent cluster nodes and peer routing

A production vagent deployment runs as a cluster of cooperating nodes. Each cluster node
(`ClusterNode`) runs an independent `MeshActorRuntime` with its own `LocalHost` actor turn
scheduler, non-blocking TCP listener, and inter-node peer routing engine.

### Node architecture and routing rules

Nodes connect to designated peers over standard TCP endpoints. When an agent on one node emits an
event `in outer`, configured forward rules route the event across the network and deliver it
directly into the target agent's mailbox on the receiving peer.

Launch a cluster node with `vagent node`:

```text
build/vagent node \
  --node <node-id> \
  --listen <port> \
  [--listen-host <host>] \
  [--peer <peer-id>=<host:port> ...] \
  [--forward <Event>=<target-node>:<TargetAgent> ...] \
  --program <source.vagent|pkg.vnpkg> \
  [--agent <AgentType>] \
  [--arg <name=value> ...] \
  [--send 'Event{field: value}' ...] \
  [--timeout <duration>] \
  [--exit-on <Event>]
```

| Option                          | Meaning                                                                  |
| ------------------------------- | ------------------------------------------------------------------------ |
| `--node <id>`                   | Logical node identity (e.g. `node-1`, `worker-east`).                    |
| `--listen <port>`               | TCP port to listen for incoming cluster peer traffic (0 for dynamic).    |
| `--peer <id>=<host:port>`       | Connect to a remote peer endpoint. Repeatable for mesh topologies.       |
| `--forward <Ev>=<node>:<Agent>` | Forward rule: route outgoing event `Ev` to agent `Agent` on peer `node`. |
| `--program <path>`              | Program source (`.vagent`) or cluster package (`.vnpkg`) to execute.     |
| `--agent <type>`                | Agent type to launch on this node upon startup.                          |
| `--arg <name=value>`            | Constructor argument passed to the launched agent.                       |
| `--send <Event{...}>`           | Initial event delivered to the agent on this node.                       |
| `--timeout <dur>`               | Maximum duration to run before orderly shutdown (e.g. `10s`, `30s`).     |
| `--exit-on <Event>`             | Event type whose emission or receipt triggers immediate exit.            |

### Worked example: Three-node distributed pipeline

The following complete program implements a distributed sensor acquisition, processing, and auditing
pipeline split across three cooperating cluster nodes:

**Checkable source; runtime predicted.** Save as `pipeline.vagent`.

```vagent
module demo:pipeline

predicate reading(batch_id: Str, sensor: Str, count: Int, raw: Int)
predicate computed(batch_id: Str, score: Int)
predicate audited(batch_id: Str, status: Str)

event StartSensor { batch_id: Str, readings: Int, raw: Int }
event SensorBatch { batch_id: Str, sensor_id: Str, readings_count: Int, total_raw: Int }
event ComputeResult { batch_id: Str, node_id: Str, total_raw: Int, scaled_score: Int }
event AuditVerdict { batch_id: Str, nodes_verified: Int, total_score: Int, status: Str }

agent SensorProducer {
  on StartSensor as s {
    emit SensorBatch{batch_id: s.batch_id, sensor_id: "node-1-sensor", readings_count: s.readings, total_raw: s.raw} in outer
  }
}

agent ComputeWorker {
  on SensorBatch as b {
    believe reading(b.batch_id, b.sensor_id, b.readings_count, b.total_raw)
    let score := b.total_raw * 2
    believe computed(b.batch_id, score)
    emit ComputeResult{batch_id: b.batch_id, node_id: "node-2-compute", total_raw: b.total_raw, scaled_score: score} in outer
  }
}

agent ClusterAuditor {
  on ComputeResult as c {
    if c.scaled_score >= 100 then {
      believe audited(c.batch_id, "VERIFIED")
      emit AuditVerdict{batch_id: c.batch_id, nodes_verified: 3, total_score: c.scaled_score, status: "VERIFIED"} in outer
    }
  }
}
```

### Running the cluster pipeline

Start the three nodes in order across separate terminals:

```sh
# Terminal 3: Start Node 3 (Auditor) on port 9003
build/vagent node --node node-3 --listen 9003 \
  --program pipeline.vagent --agent ClusterAuditor \
  --exit-on AuditVerdict

# Terminal 2: Start Node 2 (Worker) on port 9002, peering with Node 3
build/vagent node --node node-2 --listen 9002 \
  --peer node-3=127.0.0.1:9003 \
  --forward ComputeResult=node-3:ClusterAuditor \
  --program pipeline.vagent --agent ComputeWorker

# Terminal 1: Start Node 1 (Sensor) on port 9001, peering with Node 2, and trigger pipeline
build/vagent node --node node-1 --listen 9001 \
  --peer node-2=127.0.0.1:9002 \
  --forward SensorBatch=node-2:ComputeWorker \
  --program pipeline.vagent --agent SensorProducer \
  --send 'StartSensor{batch_id: "batch-101", readings: 5, raw: 75}' \
  --timeout 5s
```

Trace across the cluster:

1. Node 1 receives `StartSensor`, and `SensorProducer` emits `SensorBatch` with `total_raw: 75`.
1. Node 1's forward rule transmits `SensorBatch` over TCP to Node 2 (`127.0.0.1:9002`).
1. Node 2 delivers `SensorBatch` to `ComputeWorker`. The worker asserts `reading(...)`, doubles the
   raw count (`75 * 2 = 150`), records `computed("batch-101", 150)`, and emits `ComputeResult`.
1. Node 2's forward rule transmits `ComputeResult` over TCP to Node 3 (`127.0.0.1:9003`).
1. Node 3 delivers `ComputeResult` to `ClusterAuditor`. Since `150 >= 100`, the auditor asserts
   `audited("batch-101", "VERIFIED")` and emits:
   `AuditVerdict {batch_id: "batch-101", nodes_verified: 3, status: "VERIFIED", total_score: 150}`.
1. The `AuditVerdict` emission satisfies Node 3's `--exit-on` rule, gracefully terminating the run.

## Interactive terminal dashboard

`vagent tui` provides an interactive, terminal-based NCurses dashboard for monitoring live agents,
inspecting epistemic facts and uncertainty scores, and issuing orchestration commands in real time.

```sh
# Launch interactive dashboard with an agent program
build/vagent tui demo.vagent --agent Tracker

# Run in headless mode (dumps virtual screen buffer to stdout, ideal for tests)
build/vagent tui demo.vagent --agent Tracker --step --headless
```

**Checkable source; runtime predicted.** Save as `dashboard.vagent`.

```vagent
module demo:dashboard

predicate at(who: Str, x: Float, y: Float)
predicate status(text: Str)

event Ping
event StepTick

agent Tracker {
  var x: Float := 10.0
  var y: Float := 20.0

  on Ping {
    believe at("scout", 12.5, 40.0)
    x := 12.5
    y := 40.0
  }

  on StepTick {
    x := x + 1.0
  }
}
```

### The three functional panes

The TUI is organized into three dedicated operational panes, toggled with the `Tab` key or number
keys `1`, `2`, and `3`:

1. **Monitoring Pane (Key `1`):**
   - **Live actor table:** Lists all living agents with their `ActorPid`, short PID, agent type,
     name, and lifecycle state (`RUNNING`, `QUIESCENT`, `STOPPED`, `BUSY`).
   - **Resource metrics:** Real-time displays of remaining fuel quota, CPU cycle counts, scratch
     arena cells, and epistemic cells.
   - **Queue statistics:** Outstanding mailbox depth, staged outbox counts, delivery drop counters,
     malformed envelope counters, and active deadline timers.
   - Use `Up` and `Down` arrow keys to highlight and select an actor for inspection.
1. **Querying Pane (Key `2`):**
   - **Belief inspector:** Displays all asserted beliefs held by the selected agent, their asserting
     sources, and status flags (`current`, `retired`, `stale`, `fact`).
   - **Epistemic arbitration scores:** Inspects Bayesian epistemic metrics: uncertainty, certainty
     scores (0.0 to 1.0), and incarnation numbers computed by the Vlog unification engine.
   - **Interactive logic query bar:** Type Prolog / Vlog queries directly into the query prompt to
     interrogate the agent's live knowledge base (e.g. `?at(Who, X, Y)`).
1. **Orchestrating Pane (Key `3`):**
   - **Action repl:** An interactive prompt to drive, manipulate, and step actors.
   - **Action log:** A persistent log showing execution outcomes and dispatch acknowledgments.

| Command in Orchestrating Pane | Action performed                                                                           |
| ----------------------------- | ------------------------------------------------------------------------------------------ |
| `step [N]`                    | Advances the runtime scheduler by N turns (default 1).                                     |
| `advance <duration>`          | Advances the manual virtual clock (e.g. `advance 250ms`, `advance 2s`) to fire due timers. |
| `inject <pid> <Event{...}>`   | Injects an event into the target actor's mailbox.                                          |
| `spawn <AgentType> <name>`    | Instantiates a new agent actor under the runtime.                                          |
| `upgrade <pid> <programRef>`  | Performs an atomic live upgrade of the target actor to a new program version.              |
| `stop <pid>`                  | Requests an orderly shutdown and triggers the actor's `on Destroy` handler.                |

### Navigation and controls

| Key           | Function                                                                 |
| ------------- | ------------------------------------------------------------------------ |
| `Tab`         | Cycles through active panes (Monitoring -> Querying -> Orchestrating).   |
| `1`, `2`, `3` | Direct jump to Monitoring (`1`), Querying (`2`), or Orchestrating (`3`). |
| `Up` / `Down` | Selects previous / next actor in the monitoring table.                   |
| `Enter`       | Submits command input or opens selected actor details.                   |
| `s`           | Quick single-turn step from any pane.                                    |
| `q` or `Esc`  | Exits the TUI dashboard.                                                 |

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

| Words                                               | Role and common interaction                                                                           |
| --------------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| `module`, `import`, `as`                            | Logical module declaration/import, aliasing, event or await binding.                                  |
| `record`, `event`, `extends`                        | Typed values/events and event/capacity subtype relationships; see [records](#records-named-values).   |
| `agent`, `behavior`, `mount`, `unmount`             | Actor declaration and reusable in-actor contributions.                                                |
| `capacity`, `skill`, `native`, `implements`, `uses` | Method contract, implementation, host binding, and caller visibility.                                 |
| `install`, `uninstall`                              | Add/remove a skill; replacing a conflict removes its whole implementation.                            |
| `def`, `return`, `requires`, `ensures`              | Ordinary methods and pure call contracts; return exits a method, not an intention.                    |
| `effect`, `preempts`, `within`                      | Asynchronous native work, explicit preemption contract and deadlines.                                 |
| `var`, `let`, `none`                                | Persistent field, local declaration, optional absence.                                                |
| `if`, `then`, `else`, `not`                         | Success/failure control flow and local recovery; else associates with the nearest eligible construct. |
| `for`, `in`, `while`, `do`, `to`, `by`              | Deterministic traversal/loops and inclusive ranges; in/by also occur in message/delegation forms.     |
| `predicate`, `key`, `believe`, `revise`, `forget`   | Typed fact declaration, replacement key and self-source updates.                                      |
| `believes`, `from`, `findall`                       | Pure matching, source restriction and materialized matches.                                           |
| `tell`, `untell`                                    | Staged source-bearing belief messages.                                                                |
| `goal`, `achieve`, `perform`, `maintain`            | Goal kinds; achieve at a call site also waits for a local/delegated goal.                             |
| `succeed`, `when`, `holds`, `drop`                  | Pure commitment conditions and explicit cancellation of an intention.                                 |
| `plan`, `where`, `adopt`                            | Goal implementation, pure applicability and nonwaiting top-level adoption.                            |
| `on`, `occurrence`, `priority`, `consume`           | Event handling/binding, within-event order, and stopping later handlers.                              |
| `emit`, `in`, `for`                                 | Stage an event, choose its space, restrict recipients.                                                |
| `question`, `answer`, `ask`                         | Typed read-only remote queries and a plan's wait for their result.                                    |
| `await`, `as`, `within`                             | Wait for a fact or future event; capture the match and bound its lifetime.                            |
| `spawn`, `self`, `parent`, `inner`, `outer`         | Named actors, family relationships and space context.                                                 |
| `yield`, `fail`, `stop`, `terminate`                | Cooperate, fail computation, request a child's stop, or stop self.                                    |

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

The vagent toolchain delivers full syntax checking, compilation, multi-node clustering, sandboxed
native execution, packaging, and interactive terminal dashboard capabilities across several
commands.

| Area                                  | Status and what it means for this guide                                                                                                                                      |
| ------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Syntax, types and static restrictions | Standalone-source checking is implemented as `build/vagent check FILE...`. The runtime compiler dialect is checked via `build/vagent check-runtime FILE...`.                 |
| Internal Vortex compiler              | Programs compile to Vortex opcode graphs and execute within the actor runtime via `LocalHost` and `AgentExecutor`.                                                           |
| Running an agent                      | `vagent run FILE [--agent NAME] [--arg K=V] [--send 'Event{...}'] [--advance DUR]` runs agents on local meshes and prints outer emissions.                                   |
| Cluster nodes and routing             | `vagent node --node ID --listen PORT [--peer ID=ADDR] [--forward EV=NODE:AGENT]` runs concurrent cluster nodes with non-blocking TCP peer routing.                           |
| Cluster packaging                     | `vagent package` and `vagent package inspect` build and inspect multi-target `.vnpkg` packages with bundled libraries and Ed25519 author provenance.                         |
| Native host sandbox                   | `vagent native seal`, `approve`, and `withdraw` manage sandboxed `.vnprog` native host modules under two-phase OS lockdowns with dual shared-memory ring buffers.            |
| Terminal user interface (TUI)         | `vagent tui [FILE] [--agent NAME] [--step] [--headless]` launches the interactive NCurses dashboard for monitoring, querying beliefs, and orchestrating agents in real time. |
| Schema compatibility & upgrade        | `vagent check-runtime --upgrade OLD NEW` verifies field, container, and predicate upgrade compatibility.                                                                     |

### Command line reference

```text
build/vagent --help
build/vagent check FILE...
build/vagent check -
build/vagent check-runtime [--upgrade] FILE...
build/vagent run FILE [--agent NAME] [--arg K=V] [--send 'Event{...}'] [--advance DUR] [--turns N]
build/vagent node --node ID --listen PORT [--peer ID=HOST:PORT] [--forward EV=NODE:AGENT] --program PROG
build/vagent package INPUTS... -o PKG.vnpkg [--target TRIPLE=SO] [--bundle-lib TRIPLE=SO] [--bundle-dir TRIPLE=DIR]
build/vagent package inspect FILE.vnpkg
build/vagent native seal MODULE.so [-o PROG.vnprog]
build/vagent native approve TARGET --kind KIND [--as AUTHOR] [--ledger DIR]
build/vagent native withdraw TARGET --kind KIND [--as AUTHOR] [--ledger DIR]
build/vagent tui [FILE] [--agent NAME] [--step] [--headless]
```
