# Round 3 disposition

- Reviewed target: `design/vortex-io-redesign.md`, version 0.4.
- Round-three snapshot SHA256: `562373ff5f8e53c1ce94e0e0dc61a31ad4067bd90e261472bfb81669ddad2b02`.
- Corrected target: version 0.5; all-six round-four snapshot SHA256
  `8cac1226d23d512448e126c79b428227f760ac419b4c3b94d1f2b1702c40776f`.
- This was a focused three-critic verification round after the orchestrator found the frozen v0.4
  artifact incomplete; the unaffected writer, architect, and ZigZag critics were not launched.
- Reports total: 1 BLOCKING, 3 MAJOR, 0 MINOR. Codebase reported 1 BLOCKING/1 MAJOR; Vortex and Ergo
  each reported 1 MAJOR.

## Codebase Expert

- Actor changes had no registered P5: **Accepted.** The v0.4 target contains actor policy prose but
  only P1–P4 in the actual table. Added P5 in v0.5 with exact mesh runtime, actor wire, and VAgent
  checker/compiler/executor/adapter files; owner-boundary rationale; actor grant, staging,
  completion, and correlation costs; and a refusal fallback. No claim is made that the absent v0.4
  row existed.
- User-adjustable budget settings had no registered proposal: **Accepted.** Added P6 in v0.5 for
  `system_docs.hpp/.cpp` and host snapshot resolution, with rationale, costs, and fallback. U-4 now
  explicitly retains an immutable supplied snapshot with named backend invariants until P6 is
  approved.

## Vortex & Spool-Mesh Expert

- Actor policy had no registered implementation proposal: **Accepted.** Resolved with P5 and the
  actor-specific acceptance test. Actor completion remains an owner-delivered message-boundary
  event; no VM PC or native callback becomes portable.

## Programming Language Design & Ergonomics Expert

- Actor inline-await refusal conflicted with the distinct Submit admission action: **Accepted.** The
  v0.5 contract statically disallows direct IoService Submit and Await in atomic actor callbacks
  before those turns run. Existing VAgent handler effects remain reversible staged intents, not
  service submissions; after successful commit, the dispatcher submits once. VAgent plan effects
  that would suspend in an actor turn are refused. If an invalid dynamic path reaches Await after a
  separate Submit was already admitted, refusal does not retract that request or claim there was no
  external effect. The acceptance gate checks pre-turn rejection, one post-commit submit, and no
  resubmission after restore.

## Correction history and next step

Round two's disposition described intended P5/P6 rows as if they had been saved. Readback of the
actual v0.4 target proved those rows and the U-4 fallback were absent. This disposition records the
artifact gap rather than treating the earlier disposition as evidence. Version 0.5 is the corrected
snapshot; all six critics must review its recorded hash in round four. P1–P6 remain proposals only,
and no runtime or settings implementation was made.
