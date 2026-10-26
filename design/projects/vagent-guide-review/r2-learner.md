# R2 learner diary

**Status: NOT CONVERGED.** I could follow the current ordered path and its new stopping points, and
I did not expect runtime traces from the checker. One remaining transfer gap is noted at the end.
This is a learner report, not a semantics audit: I read only `docs/vagent-user-guide.md` and did not
inspect the design, implementation, code, or earlier reviews.

## First counter and the checker boundary

The guide now gives me an ordered path: mental model → build/check → first source;
values/events/conditions → one-handler checkpoint; search/rollback;
beliefs/goals/waits/questions/delegation; composition; workshop. Its example captions say “Checkable
source; runtime predicted,” name a file, and tell me to expect `FILENAME: checked`. The section
explicitly says the source fences are checked by the guide test, but their stories are not executed.
That is a good stopping point for this build: write or edit a unit, check it, and treat state/timing
traces as predictions until an executor and harness exist.

I ran the prescribed headless/XDG setup in the worktree. `build/vagent --help` printed:

```text
Usage: vagent check FILE...
       vagent --help
       vagent --version

Check each standalone vagent source unit for syntax and static errors.
Use - for standard input. Cross-file module loading, Vortex lowering,
agent execution and the REPL are not implemented in this iteration.
```

The guide's baseline command returned exactly:

```text
tests/samples/vagent/mini_01.vagent: checked
```

with exit status 0. I did not rebuild the already available frontend.

## Checkpoint: counter and Bool

I copied the complete `counter.vagent` to `/tmp`, changed the Add handler as instructed, and checked
it. Output was `/tmp/r2-counter-negative.vagent: checked` (exit 0). The guide's predicted field is 3
after Add(3), then still 3 after Add(-2): the condition skips the second write, and the handler
succeeds. The checker only accepted the edited source; it did not deliver either event or inspect
`count`.

I then replaced the documented explicit Bool comparison with `if enabled then ...`, keeping the
guide's Enable and Work declarations. Exact output:

```text
/tmp/r2-switch-bad.vagent:7:16: TypeMismatch: a Bool condition must compare with '== true' or '== false'
```

Exit status was 1. Restoring `enabled == true` produced `/tmp/r2-switch-good.vagent: checked` (exit
0). This is the checkpoint's concrete, observable lesson today; only the counter values are
future-executor predictions.

## Search and nested rollback transfer

For `Routes`, I predict budget 3 and route `second`: the failed first comparison backtracks `<-` to
5; the second arm sets it to 3. The checker accepted the copied source, but did not demonstrate this
trace.

I followed the nested-block table without consulting anything else. The separate `budget := 6` plan
statement succeeds. Inside the following block, the speculative write to 1 is undone when its
comparison fails; the next arm selects `second` and leaves budget at 3; `budget := 9` then succeeds.
The block's final `fail` makes the entire block statement fail, undoing its writes back to the
statement-entry value 6. The earlier sibling statement stands because this is an ordinary plan
failure, not a handler failure or hard turn failure. The predicted final budget is therefore 6. If
this were a handler, the handler boundary would return it to 5; a hard turn failure could also undo
the earlier plan statement. `/tmp/r2-nested-rollback.vagent` checked successfully (exit 0), but that
output does not establish any rollback result.

This nested example now gave me a usable rule: locate the enclosing failed statement, then the
enclosing handler/turn if applicable. The table makes the scopes concrete instead of leaving me to
reconcile “irreversible” with outer rollback.

## One wait and deadline edit

I checked the one-intention `one-wait.vagent` as `/tmp/r2-one-wait-2s.vagent`; it returned `checked`
(exit 0). I predict the table as written:

| Selected input/work                 | Prediction, not observed execution                             |
| ----------------------------------- | -------------------------------------------------------------- |
| Start at time 0                     | WaitOnce is ready; `complete` is false.                        |
| Select its plan at time 0           | Await suspends until Done or deadline 2s; `complete` is false. |
| Done at 1s                          | The wait captures the event and makes the intention ready.     |
| Select continuation at 1s           | `complete` becomes true; perform goal succeeds.                |
| No Done; select deadline wake at 2s | Wait and goal fail; `complete` remains false.                  |

I changed only `within 2s` to `within 3s`, as prompted. `/tmp/r2-one-wait-3s.vagent` checked (exit
0). My prediction: the Start and plan-selection rows, and a Done at 1s, are unchanged; the timeout
wake moves from 2s to 3s. The changed deadline is a source edit the current CLI can accept, not a
timer I can advance.

## Delegation before workshop

I followed the separate `delegation.vagent` before attempting the synthesis unit.
`/tmp/r2-delegation.vagent` checked (exit 0). I predict: Initialize spawns Courier; Start adopts
Send; Send suspends at `achieve Deliver() by courier`; after Courier accepts and finishes the
perform goal, Send resumes and sets `finished` true. If delegated work is refused, fails, or times
out, the wait fails and `finished` stays false; timeout stages cancellation. This example has no
belief message and no stock query, which made the delegated outcome easier to distinguish. None of
those turns can be selected with the current CLI.

## Sources and workshop message orders

I read the A/B source table as one fact with a changing source set, not one independent fact per
source:

| Step                                    | Sources at B | Exists? |
| --------------------------------------- | ------------ | ------- |
| A tells B `ready("j-1")`; B receives it | A            | Yes     |
| B also believes it                      | A, B         | Yes     |
| A untells it; B receives the withdrawal | B            | Yes     |
| B forgets its own assertion             | none         | No      |

This clarifies why B's `forget` cannot erase A's assertion. The A/B trace is labeled as a predicted
message/state trace, not CLI output; `tell` and `untell` aren't commands I can run today.

The workshop source checked as `/tmp/r2-workshop.vagent` (exit 0). For 40 units and count 3, I
predict the stock answer lets Handle proceed. It then delegates Fetch and waits for
`delivered(order_id)`. The tell and delegated outcome are separate messages. If the **outcome
arrives first**, Handle resumes, reaches await while the fact is absent, and suspends; later receipt
of the tell makes the fact true and resumes Handle, which increments completed and stages Completed.
If the **fact arrives first**, it is present by the time the successful delegation resumes Handle;
the belief await sees it immediately, then completed increments and Completed is staged. Both traces
predict completed = 1 once the continuation runs. The order-key prevents a delayed fact for another
order from satisfying this wait. These are paper traces; check acceptance did not test either
message order.

For stock 2 and count 3, I predict `available < count` succeeds, so its `then fail` makes Handle
fail and no Fetch is delegated. If the stock ask times out and uses fallback 0, the same branch is
selected. No Shortage event is declared or emitted in this revised unit; the guide says the goal
lifecycle reports failure. This was clear after the new explicit stock-failure paragraph.

## Additional learning gap

The current path, captions, example filenames, expected check output, and stopping point are enough
for me to distinguish what I can practice today from what I can only predict. The ordered journey
from one wait to delegation and then workshop also supported transfer without requiring an unbuilt
runtime.

One small workshop simplification remains implicit: `Warehouse(units)` implements
`answer stock(item)` as `{ units }`, so the `item` argument is ignored and every item receives the
same count. The guide carefully says it is not real inventory and explicitly warns that there is no
stock reservation, but a first-time reader may still wonder whether item matching is intentionally
omitted or accidentally broken. Add one sentence that this toy warehouse returns the same fixed
stock for every item (or make the unit return stock by item). This is actionable but does not block
understanding the stated order/fact/outcome story, so I do not mark the guide converged.

All accepted units above are checker acceptance only. The sole intentional rejection was the
bare-Bool condition, with the exact diagnostic recorded above. I did not try the future `run`
command or infer execution from successful checks.
