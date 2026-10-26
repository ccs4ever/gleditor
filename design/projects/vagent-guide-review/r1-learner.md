# R1 learner diary

I followed `docs/vagent-user-guide.md` from its mental model through the workshop section, using
only that guide and the available `build/vagent`. I did not read the language design, implementation
record, source, or other reviews.

## What I could do today

The guide was clear that this checkout exposes a frontend-only checker. I used its headless and XDG
exports and ran:

```text
build/vagent --help
Usage: vagent check FILE...
       vagent --help
       vagent --version

Check each standalone vagent source unit for syntax and static errors.
Use - for standard input. Cross-file module loading, Vortex lowering,
agent execution and the REPL are not implemented in this iteration.
```

Then `build/vagent check tests/samples/vagent/mini_01.vagent` returned:

```text
tests/samples/vagent/mini_01.vagent: checked
```

I did not rebuild: the guide says to build on a fresh checkout/worktree, and the existing binary was
present. The check result is evidence only that the frontend accepted the source; it says nothing
about its runtime trace.

I copied the first counter, rollback example, an await/yield plan, the tagged-string scanner, and
the workshop unit into independent scratch files under `/tmp`. Each passed `build/vagent check`:

```text
/tmp/vagent-counter.vagent: checked
/tmp/vagent-rollback.vagent: checked
/tmp/vagent-wait.vagent: checked
/tmp/vagent-parse.vagent: checked
/tmp/vagent-workshop.vagent: checked
```

This let me practice editing and checking units, but none of the described state changes, waits,
messages, parses, or goal outcomes can be observed with this command. The tagged-string section
explicitly calls out that the scanner registrations are not supplied by frontend acceptance. The
status table similarly makes the first counter trace, rollback rules, and workshop scenario
specified behavior rather than an executable tutorial in this iteration.

As a small negative check, I changed the counter-style condition to `if enabled then ()`. The
checker gave the documented kind of diagnostic:

```text
/tmp/vagent-bool-condition.vagent:3:22: TypeMismatch: a Bool condition must compare with '== true' or '== false'
```

## Transfer exercises and my predictions

- **Counter/handler:** I changed the handler to add `a.amount * 2` to `count`. From the guide's
  event binding and ordinary arithmetic rules, I predict a starting count of 0 and `Add{amount: 3}`
  leave 6; a second amount 2 leaves 10. A question answer reads the current count when asked. I can
  check the changed source, but cannot deliver events or query it today.
- **`:=` versus `<-`:** In the supplied `Routes`, the first arm tentatively sets `budget` to 1, then
  `budget > 2` fails. Search backtracking restores 5 before the second arm sets it to 3; the
  selected string is `second`. I infer the rollback is local to the expression's choice point. The
  guide also says a later failure of the enclosing bounded statement/handler/turn can undo a `:=`
  write, so “irreversible” is easy to overread. Static acceptance does not prove the trace.
- **Goal plus wait:** I wrote a `WaitFor` perform goal with `await ready(id) within 2s` followed by
  `yield`; the checker accepted it. I predict an already-present belief satisfies the await
  immediately, otherwise the intention suspends until the fact or deadline, then yield makes it
  ready later. I could not establish timing, event delivery, or resumption from the checker. The
  guide explains waits in separate sections but offers no executable path to step through this
  example.
- **Ask versus effect:** I understand `ask peer.question(...)` as a typed, read-only query whose
  result comes from another agent's answer and whose plan statement waits for that agent's
  turn/deadline. An `effect` call stages host work through an installed native binding, suspends a
  plan until completion/failure/deadline, and may have an external consequence that rollback cannot
  undo. A native declaration alone does not connect a device. I can distinguish the contracts in
  prose, but there is no live peer, host binding, or device to compare.
- **Tagged string:** For `ID:bolt;`, I predict the scanner matches and consumes `ID:`, extracts up
  to but not including `;`, and assigns `bolt`; a missing delimiter fails the handler, preserving
  the old `tag`. The snippet checks, but the guide explicitly warns the scan registrations are not
  actually provided merely because the import is accepted. Thus I have a rule-based prediction, no
  observed parse.
- **Workshop:** I can follow the intended sequence: initialize children; handle adopts; dispatcher
  asks warehouse, delegates Fetch, awaits an order-keyed belief, then completes. I understand zero
  as the chosen fallback for unavailable stock, and that message/fact delivery order can vary. I
  cannot run or inspect any of it now. I noticed a wording trap: the trace says “The shortage
  condition fails, so the total if does nothing,” after which it delegates. A newcomer can read
  “shortage condition fails” as a plan failure, although the code's `if available < count` condition
  is false when stock is sufficient, and it is the `if` branch that does nothing. Name the condition
  explicitly (for example, “the `available < count` test is false”) to make this step unambiguous.
  The earlier paragraph should also say directly that low stock makes the branch fail the plan and
  its enclosing `if` rollback scope may retract the staged `Shortage` emission; the later warning
  does eventually explain that.

## Where I got stuck or wanted a next step

1. The guide teaches rich runtime behavior while the only runnable command checks syntax and static
   restrictions. It does disclose this prominently, but after each accepted exercise I had no
   concrete next action that could demonstrate whether my prediction was right. A tiny labeled
   “today / once the adapter exists” note beside the exercises, plus an expected checker-only
   outcome, would help set the stopping point.
1. `:=` is first described as “irreversible” within expression search, then the rollback table
   broadens the boundary to statement, handler, and turn. I could reconcile the rules, but a compact
   nested example with a failed inner branch followed by a failed outer statement would make the
   boundary much easier to retain.
1. The goal exercise says to “write a simple goal+plan and wait”; I could write one and infer
   behavior, but the guide's wait example adopts two goals and has a belief handler. A minimal
   single-goal example that states the initial fact state, input/time, and predicted intention state
   would be a better first transfer step. The testing section later describes that format, but it
   arrives much later.
1. I wanted a concrete sentence distinguishing **checkable now** (source shape and implemented
   static rules) from **specified but not demonstrated now** (runtime scheduling, rollback traces,
   parsing registrations, inter-agent outcomes, and host effects). The implementation-status table
   covers the distinction; repeating a short version before the exercise traces would reduce the
   chance that “checked” is mistaken for “worked.”

I found no checker failures among the guide-derived units. The one rejection was the intentional
bare-Bool condition above. I did not treat any accepted source as proof of runtime semantics. These
gaps are teaching/verification gaps for a frontend-only stage, not evidence that the guide's future
runtime behavior is wrong.
