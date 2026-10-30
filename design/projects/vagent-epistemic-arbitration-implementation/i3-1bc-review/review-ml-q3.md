# ML gate confirmation: open question 3 (profile on a bound source's first refusal)

Scope: head `616840cc` on accepted I3-1 (`3f8815f9`), judged as health and calibration data.
`VagentEpistemic*`: 101 tests pass (headless). No repository file changed; probes and mutants are in
`/tmp/i3-1bc-ml/r2/`.

## Probe results (`F.Q3*`)

| probe | result |
| --- | --- |
| bound source, bad units as its first event | profile created at incarnation 1; `content_refusals` 1, `refused_count` 0, `reject_count` 0, `last_contact` set, `last_receive` unset |
| a good commit afterwards | same profile found; counters unchanged; `last_receive` now set |
| refused event from a principal nobody bound | refused at the door, no profile created |
| relayed bad copy as the first event | profile created; `content_refusals` 0, `refused_count` 1, `last_contact` unset |
| then the identical copy direct | charged `content_refusals` 1 and contact set (the round-3 masking minor does not reproduce at this head) |
| `max_sources` 2; two bound sources send only bad events | both hold a slot with `content_refusals` 1 |
| a third, healthy source's first valid reading | `EpistemicBudget`, nothing stored |
| third source's bad event while full | stat `refused_EpistemicBudget` rises; no profile, charge lost |
| retire one source, resend the third's same bad event | not charged (charge-once treats it as a retry); no profile |
| a new bad event from the third source | profile created, `content_refusals` 1 (the first refusal never counted) |

Mutation check (unit tests only; I did not re-run the fuzz stand-in):

| mutant | killed |
| --- | --- |
| budget check removed from the shared profile routine | yes (3 tests) |
| profile created at constant incarnation 1 instead of the binding's | **survives** |
| bookkeeping turn not admitted against the cell ceiling | **survives** |

## BLOCKING

None.

## MAJOR

None. Counters at creation are right for calibration: all four counters start at zero and only the
note's counts are added, so a first-refusal profile equals what a commit would have made plus the
refusal; the gate's `reject_count` and streak stay apart; a direct content refusal shows contact,
a relayed one does not; host conditions and unbound principals create nothing (the existing
`HostConditionsAreNeverCharged` test still passes). A source wrong from its first event no longer
looks clean when it first commits, which was the point of the question.

## MINOR

1. **Refusal-only sources now hold `max_sources` slots.** Probe: two sources that only send bad
   events fill a budget of 2, and the healthy third source is refused `EpistemicBudget` on every
   reading. Before this change a source that never got anything in held no slot. The host bounds
   the number of bound sources, not the budget (`bindSource` does not compare them), so a pair of
   misconfigured devices can lock out a good one. Either count only profiles that have committed
   or hold a reading toward the budget, or document that `max_sources` must be at least the number
   of bound sources, and have `bindSource` check it.
1. **Budget-refused charges are lost, and charge-once then keeps them lost.** Probe: the third
   source's first bad event is dropped with the turn (`EpistemicBudget` stat only); after room
   appears, resending that event is a "retry" and is not charged, so the profile's first counted
   refusal is its second event. The host erased the charge before the turn ran
   (`agent_executor.cpp` ~:3770). Re-queue a charge whose bookkeeping turn ended `EpistemicBudget`
   or admission, or log that outcome so charge-once does not treat it as held.
1. **Survivors in the test set.** No test binds incarnation 2 and refuses first (so creation at the
   binding's boot is untested beyond 1), and none exercises the new admission of the bookkeeping
   turn at the cell ceiling. Add both. The tests also do not cover a relayed refusal creating a
   profile (my probe does); pin that it creates one with `refused_count` only.
1. **The fuzzer invariant was weakened.** `fuzz_epistemic_ingress.cpp` now requires only that
   beliefs are unchanged by a pre-queue refusal, since such refusals may create a profile. That
   removes the check that `UnknownSource` writes nothing to health. Restore a narrower check: a
   refusal of an unbound principal leaves the profile set unchanged.
1. **Cost of refusal turns.** The bookkeeping turn is now admitted, so each one runs the
   ceiling-bounded C++ admission walk (up to 24576 cells), and a refused principal that holds no
   profile posts a note per refusal (no coalescing across turns, since the charge is erased at
   drain). Refusal turns do not count toward `ingest_per_wake`. Bounded by the bindings and
   previously a cheaper turn; the hardware persona should confirm it.
1. **Health shape for I3-2.** A refusal-only profile has `last_receive` unset and a `last_refusal`
   code; I3-2 must treat "profile with no receive" as a defined state. The carried minors from
   round 3 (own-flood eviction, lifetime counters, health-at-ingest stamp untested, turn-level
   repeats charged again) are unchanged by this commit.

Tally: BLOCKING 0, MAJOR 0, MINOR 6.

Verdict: PASS
