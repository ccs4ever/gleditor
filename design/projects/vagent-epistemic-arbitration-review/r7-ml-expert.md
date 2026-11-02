# Round 7 ML Review — v1.4 Focused Check

## R6 findings status

- **R6 MAJOR 1 — superseded labels remained in Beta/SGD state: CLOSED.** The active ledger permits
  one effective adjudication per target/epoch; a correction needs a new event ID and predecessor
  CAS. Beta is recomputed from the epoch prior and active ledger. SGD resets to the epoch
  initialization and replays effective labels in original first-accept order, placing a revision at
  its predecessor's position. Replay is scratch-built and atomically published; budget/numeric
  failure leaves old state intact. Old records outside the bounded replay horizon are refused, not
  approximated by a compensating label.
- **R6 MAJOR 2 — learner state/domain/epoch matching unspecified: CLOSED.** Domain-calibration
  records are keyed by full source identity/incarnation, predicate signature, domain/schema,
  calibration/model epoch and feature schema. Observations and verification events carry the
  corresponding key; delayed labels/completions must match the live learner exactly or expire. No
  profile-wide shared weights/counts are implied.
- **R6 MINOR — LCB and age-decay composition unclear: CLOSED.** §3.4 orders learner selection, Beta
  mean/prediction, optional Beta LCB, age decay, logit clamp and pooling. It states age does not
  change Beta counts and the result is only a warrant heuristic.

## Replay/lifecycle audit

Reviewed revision identity/CAS, one-active-label semantics, correction replacement of both y and q,
Beta recomputation, deterministic SGD replay with fixed scaler and label order, bounded retained
history, fuel/cell budget refusal, atomic publication, epoch reset/expiry, cached-score/continuation
invalidation, and retry/redelivery outcome handling. No new material mathematical or lifecycle
defect found in the revised rules. The fixed LCB/decay order is coherent with the stated heuristic;
contextual bandits, matrix fusion and exact posteriors remain deferred as approved.

## Tally and verdict

- BLOCKING: 0
- MAJOR: 0
- MINOR: 0

VERDICT: CONVERGED
