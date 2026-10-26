# Programming teacher review — R2

## Decision: CONVERGED

Reviewed the frozen guide at SHA-256
`489fd5166c1de5d04a83d1058e52a0431234a8b37e2ddf4622e8d76a5c8e1b18`. The three R1 teaching gaps are
addressed. I found no remaining actionable gap for the stated audience: programmers who know
variables, functions, loops and basic types, but have no actor, logic-programming or goal-oriented
programming background.

## Review of the R1 findings

1. **Success and failure semantics:** The new `Try` and feedback table before the Switch example
   distinguish a value of `false` from expression failure, and show how a failed comparison affects
   an `if` with and without `else fail`. That is the needed bridge before the alternative-search
   chapter. The first checkpoint also separates predictions from what the checker can demonstrate.
1. **Belief provenance and waiting:** The source exercise correctly traces two sources, keyed
   replacement per source, withdrawal, and self-forget. The follow-up `from self` query edit has a
   clear accepted-source result and a predicted non-match. The three timing cases correctly
   distinguish an already stored belief from an event wait registered only after execution reaches
   `await`; the replacement edit and answer table reinforce the transfer.
1. **Independent composition and debugging:** The shortage extension now asks for an unsolved
   modification before giving the code. Its criteria cover both low stock and the zero timeout
   fallback; the hint directs attention to statement rollback boundaries. The sample solution stages
   the event in one successful statement and fails in the next, correctly preserving that earlier
   output under ordinary plan failure. It also identifies the later `GoalFailed` selection wake and
   explicitly distinguishes a hard turn failure that would undo staged output.

## Teaching sequence and feedback

The “Who this guide is for” section names prerequisites and sets observable learning outcomes. The
reading path proceeds from a small handler and static checker exercise to expression semantics,
rollback, beliefs, waiting, messaging and a synthesis task. New “Try” prompts ask learners to commit
to a prediction before the answer tables, while captions and answer text consistently distinguish
checker evidence from language-contract predictions. This supports both correcting misconceptions
and knowing when a result still needs runtime verification.

I walked the new scenarios against the guide’s stated rules. In the source exercise, after A changes
its keyed value and B withdraws its old assertion, A's `reading("left", 7)` remains; R's self-query
does not match. In the wait exercise, the belief wait can see a retained fact, while the event wait
misses an event delivered before its suspension. In the workshop extension, both low stock and
fallback 0 stage Shortage, fail the current plan without delegation, leave `completed` unchanged,
and lead to goal failure when the only plan is exhausted. Those outcomes are internally consistent
with the surrounding rollback and wait explanations.

The remaining runtime stories are labeled as predictions, and the guide does not imply that accepted
source proves execution. The new examples provide enough guided and independent practice to continue
without converting the user guide into a full course. No additional edit is warranted from the
teaching perspective.
