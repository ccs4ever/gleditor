# R2 student review: fresh programming languages pass

I read only `docs/vagent-user-guide.md`; I did not consult any other guide, design, implementation,
or review material. The frozen guide's SHA-256 is
`489fd5166c1de5d04a83d1058e52a0431234a8b37e2ddf4622e8d76a5c8e1b18`.

## Audience and objectives

The stated audience fits me: I know variables, functions, loops, and basic types, but not actors,
logic programming, or goal-oriented programming. I came away able to distinguish ordinary values
from success/failure, explain that agents own private state and take turns, and trace the major
rollback/suspension boundaries. I can describe the example language without claiming the checker
executes it.

## Try exercises and observations

**Records and Coordinates.** Before reading the feedback, I predicted that changing
`last_x := position.y` makes `last_x` become 4 for destination `Point{x: 3, y: 4}`. The record is
copied into the field; `.y` reads its second named component. I independently checked that edited
source: `build/vagent check` accepted it. The value 4 is a prediction, not a runtime observation.
The lesson makes the key type idea clear: a `Point` is a typed record value, while the enclosing
`Move` event gives the value delivery meaning.

**Success mini-trace and first checkpoint.** With `enabled` false, I predicted:

- `enabled` succeeds with the value `false`, although the checker does not permit it as a bare
  condition.
- `enabled == true` fails because there is no matching value.
- `if enabled == true then ()` skips the body and succeeds with unit.
- Adding `else fail` makes the surrounding `if` fail.

When enabled is true, the comparison succeeds with `true`; both `if` forms then run their body, with
the no-else form producing unit. For the counter checkpoint, Add(3), then Add(-2), leaves count at
3; the negative amount skips the update but does not fail the handler. The checker can establish
source acceptance/rejection, but not those field values. These are good first steps from familiar
Bool control flow toward success as a separate evaluation outcome.

**Belief source/key exercise.** For the five deliveries, my trace was:

1. R has `reading("left", 4)` from A.
1. The same fact has sources A and B.
1. A's keyed value 4 is replaced by A's value 7; B still asserts 4.
1. B's untell removes B's 4, leaving A's 7.
1. R's `forget` removes only R's assertion, which it does not have, so A's 7 remains.

Thus step 3 does not replace B's assertion and step 5 does not clear the remaining fact. `from self`
does not match the final state because the only source is A, not R; the conditional skips the
assignment. I independently checked the edited handler and the checker accepted it. The source/key
exercise made the non-map-like aspect click: the key selects a source's replacement slot, and the
fact's existence can outlive one source.

**Wait timing and belief edit.** My three-case predictions for Waits were:

1. Ready before Start: the belief wait finds the fact; the event wait still needs an event delivered
   after it suspends.
1. Both plans suspend before Ready: the Ready handler records the fact and the event also matches
   the event wait; both become ready to resume.
1. Start, then Ready before the event plan reaches `await`: the belief wait can find the retained
   fact; the event wait misses that already-delivered event and needs a later one.

Replacing the event wait with `await ready(job_id)` makes it a belief wait. The retained fact can
then satisfy it in cases 1 and 3, just as in case 2. I checked that source edit successfully. All
schedules and state changes are predicted only. The distinction I had to keep straight is that
`adopt` creates ready work but does not register an event wait; an event is captured only once
execution reaches the event await.

## Independent workshop extension

Before reading the solution, I added `event Shortage { order_id: Str }` and predicted that the
shortage branch needs two plan statements:

```vagent
if available < count then emit Shortage{order_id: order_id}
if available < count then fail
```

My reasoning was that the first conditional succeeds after staging Shortage, while the second
statement independently fails. Under ordinary plan failure, the earlier successful statement stands;
no Fetch follows, `completed` stays 0, and Handle later ends failed when its only tried plan has no
untried alternative. A hard-failed turn could roll back the staged event. Putting `emit` and `fail`
inside one failing nested block would undo the event with that statement.

For warehouse units 2 and order count 3, the answer 2 makes `available < count` true and the two
statements produce one Shortage for that order id, then plan failure. If the stock question times
out and its fallback is 0, the same comparison and branch are selected; 0 does not prove the real
inventory is empty. With sufficient stock, neither shortage conditional fails, so the original
Fetch/wait/completion path continues. I checked my modified complete workshop source; it was
accepted. That confirms syntax/static rules, not event delivery, staged output, or goal outcomes.

The guide's acceptance criteria and answer matched my independently formed solution and traces. The
hint correctly directs attention to statement boundaries without giving away the edit. I can now
explain why this small example composes a typed question, a delegated goal, a source-bearing belief,
and ordinary plan rollback without treating them as one synchronous call.

## CLI limits and conclusion

I used the existing `build/vagent check` CLI headlessly with the guide's XDG redirects. It accepted
my four scratch edits (Coordinates, `from self`, belief wait, and Shortage). None of the predictions
above are live runtime results; the guide clearly says source checking does not execute agents or
advance turns.

**CONVERGED.** I found the audience, objectives, Try/feedback loops, and requested new exercises
sufficient for this learner. The prompts make me predict before showing feedback, then separate
checker evidence from runtime prediction. I found no actionable learning gap in this pass.
