# R1 student review: programming languages

I read only `docs/vagent-user-guide.md`. Its SHA-256 matched the frozen input:
`6adf9a4e099fc1890f78b6d9080de7c7604b8eac53e257a2f1092c94b2eb8667`.

## Starting point

I know variables, functions, and loops in a conventional language. I had not used goal-directed
search, belief bases, or actors. My first useful distinction was that a vagent expression does not
always return a value: it may succeed (possibly with a value), offer another result, or fail. A
condition checks success; it is not an implicit Bool conversion. That explains why `false` can be a
successful value and why `enabled` is rejected as a condition.

The guide also gave me a workable actor picture: each agent has private state and handles one turn
at a time; handlers react to events, while plans are resumable goal work. A wait ends a plan segment
and saves its continuation, so another event/agent can run before the plan resumes. I initially
pictured `achieve` as an ordinary function call; the guide's comparison of `adopt` (start work and
return an id) with `achieve` (start/wait as a plan statement) corrected that.

## Checkpoints and transfer exercises

I followed the guide's counter and Bool checkpoint on scratch source. The counter edit's prediction
is that Add(3), then Add(-2), leaves count at 3: the failed `if` body skips the update, but does not
fail the handler. This is a paper prediction; the available command cannot run it. For the
deliberate Bool edit, the checker emitted the documented
`TypeMismatch: a Bool condition must compare with '== true' or '== false'`. I first forgot to
declare the scratch event and got an additional `UnknownName`; adding the event made the intended
rejection unambiguous.

I then made three small scratch transfer checks:

- **Success vs Bool:** a Bool can be a value regardless of whether it is true or false;
  `if enabled == true` checks a match. The guide answers why the shorthand condition is rejected and
  distinguishes `not expression` from Bool negation. The checker confirms the rejection, but cannot
  demonstrate runtime branching.
- **Search and rollback:** in the route example, the first arm tentatively writes 1, fails its
  comparison, and restores budget before the second arm writes 3. The right arm's string supplies
  the selected value. The nested example helped me distinguish branch rollback from rollback of a
  whole bounded statement and from handler/turn rollback. I can now predict the final budget 6 in
  the nested ordinary-failure trace.
- **Belief source and key:** a key replaces only this agent's prior keyed assertion; the fact can
  have multiple sources. `forget` removes my assertion, while another source keeps the fact alive.
  The guide's source table and the `reading` example answer this. I had initially conflated a belief
  with a map entry or shared variable; source tracking is the important extra semantics.
- **Goal wait:** `adopt` schedules a top-level intention; `achieve` in a plan waits for the child
  outcome. A perform goal succeeds when its plan completes. `await` instead waits for a matching
  belief/event (and an event wait is future-only). The timeline and `waits` example answer which
  events/facts count. I had initially assumed an event delivered before an await would be queued for
  it; the guide says it is not.
- **Question plus delegation:** I composed an `ask` with a typed fallback, a delegated goal, and an
  await of the courier's separately told belief. The checker accepted this scratch unit. My
  predicted order is: ask suspends for an answer; the dispatcher then delegates and suspends; the
  courier tells the belief and completes; the dispatcher resumes for the goal outcome and then
  either finds the belief already present or waits for it. The workshop section explicitly traces
  both message orderings. I understand that the question answer, delegated goal, and belief are
  three distinct communication paths, not one RPC returning a complete result.

Commands used (headless, XDG redirected per guide): `build/vagent --help`, the documented sample
check, and `build/vagent check` on scratch units under `/tmp/vagent-student/`. The help output and
guide agree: this binary checks static source only. No predicted state trace above was treated as
observed runtime behavior.

## Outcome

**CONVERGED.** I could find answers to each requested concept in the guide, and the small
composition exercise is already supported by its workshop trace. I found no concrete educational gap
that warrants a guide change. The material makes the unusual semantics explicit, gives small
examples before synthesis, and labels runtime stories as predictions.
