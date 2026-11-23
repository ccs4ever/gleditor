# R3 review: VAgent Unit result and host outcome event

Target verified: `design/vortex-io-frontend-examples.md`, SHA-256
`043f09854ec8f11b62aaa2e5a43e9d8634e277eb708fd4f1cc7e07ffde466c30`.

Scope: VAgent effect return type and proposed later owner-event mapping only. Reference:
implementation plan §§3.2, 9–10; current VAgent effect syntax, executor, and adapter.

## Findings

No blocking or major finding in the narrowed contract.

## Assessment

- `effect def write(bytes: Bytes): Unit` is consistent with the VAgent effect-call contract: the
  effect statement suspends, and `Unit` does not pretend the source receives an I/O receipt or
  outcome cell. The example now explicitly says that `write(bytes)` stages the intent and does not
  bind a receipt. The source body is therefore not represented as inspecting I/O status.
- The staged actor path matches the plan's division: ActorTurn only records immutable intent; after
  commit, the adapter journals and waits for owner acknowledgement before service admission under
  `DurableEffect`. It does not put an await inside the atomic actor turn or pass mutable
  `WorkerContext` to an I/O worker.
- The proposed owner-side correlation retains the shared protocol value without converting it into
  `Unit`: every admitted request carries the complete terminal `OutcomeCell`; pre-admission refusal
  remains the distinct `Rejected` value. This preserves `Partial`, failure/cancellation/timeout
  progress and uncertainty, `Conflict`, and `Unknown` without automatic retry. Write outcomes
  correctly carry no payload.
- Using `EffectDone` as the terminal-delivery event is explicitly defined to mean “finished,” not
  “successful.” The original status remains in the managed outcome cell. The refusal path remains
  `EffectFailed` with the full rejection. Plan §10 requires the managed outcome to travel in a later
  correlated owner event and says `EffectFailed` carries typed refusal/failure data; this proposed
  split is compatible so long as the host consumer branches on the outcome tag and does not treat
  `EffectDone` as success. The document states that the field mapping is proposed adapter behavior,
  not existing runtime behavior.
- The distinction from today's implementation is properly surfaced: current `EffectFailed` carries a
  textual cause into the else ladder, and the language has no outcome-cell accessor. The example
  confines inspection to a future host-side event consumer and does not claim current VAgent source
  can inspect the full I/O result. No new runtime change is implied by this review.

## Verdict

**Accept for the requested VAgent narrowing.** `Unit` describes the source effect statement, while
the later host event carries the separate shared I/O outcome. The mapping preserves typed status and
progress and is clearly marked as proposed glue rather than current VAgent behavior. No target edits
made.
