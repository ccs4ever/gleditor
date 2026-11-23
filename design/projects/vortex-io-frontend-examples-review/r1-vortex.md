# R1 review: Vortex execution and continuation

Target verified: `design/vortex-io-frontend-examples.md`, SHA-256
`2b90cb6a675e12d1ace92a4b222744bc44fa3ce9e4541f8146693c0563034d93`.

Reference: `design/vortex-io-implementation-plan.md` §§1–3, 9–10; current Vortex VM and frontend
runner APIs.

## Findings

### MAJOR — outcome access is not fully lossless across the six examples

The shared fixture correctly distinguishes admission refusal from terminal outcomes, and the
examples consistently keep `Pending` in runner state. However, several examples reduce outcomes to a
status-only case or application helper without guaranteeing preservation of the outcome cell's full
fields. VQL's `failed($cause)`, bare `cancelled`/`timed_out`, `unknown($detail)`; VPL's status
alternatives; and VLace/VIcon/VAgent's prose do not consistently carry optional payload, progress,
or the tag-specific fields. §3.2 permits retained payloads on `Failed`, `Cancelled`, and `TimedOut`,
requires progress to remain visible, and defines `Unknown{lastKnown}`,
`MessageTooLarge{required, disposition}`, `Stale{expected, observed}`, and
`Conflict{expected, observed, progress}`. VPL also omits `MessageTooLarge` and `Stale` from its
illustrated terminal match. Make each example's inspection total over the operation's legal variants
and pass the full typed outcome (or all fields) to its handler. The managed refusal sum should
remain a separate case from `IoOutcome`.

Evidence: plan §3.2's `IoOutcome` definitions and operation-result table; §10 requires that outcomes
remain inspectable values and payload access be status-gated.

### MAJOR — VLace's chosen bridge does not match the settled §10 API

The companion intentionally says VLace uses the shared cell protocol, but its
`performIO(submit ReadCap ...)` path has no corresponding settled plan surface. §10 instead
specifies `readBytes endpoint offset max` as a lazy `IO` action, `Driver::runIOAsync(action)`, and
`IoRunResult = Done(IOValue) | Parked{IOContinuation, RequestCell}`. The example calls `submit`
directly and then describes `Done` as containing the managed submission sum, while later saying the
driver awaits the request. This leaves action-to-submit, await, and `IOValue` mapping ambiguous and
effectively changes the accepted plan. Align either the example or §10: spell out that the IO action
is only a facade lowered by the runner to the exact common Submit/Await cell protocol, define what
`Done` contains, and show how the same continuation resumes; alternatively retain the plan's
dedicated surface without introducing a second outcome/request model.

Evidence: plan §10 common bridge contract and VLace row; §3.1–3.2 canonical managed request/outcome
protocol.

### MINOR — VAgent effect event mapping needs a total outcome rule

The example says `EffectDone` carries the managed outcome and `EffectFailed` carries refusal/failure
data, then discusses partial, cancellation and unknown results without specifying which event
carries each. Since terminal outcomes are values rather than logical failure, define the event
discriminator/correlation rule so every terminal `IoOutcome` is delivered as its original managed
value and is never collapsed to a generic failure string/event. This is particularly important for
`Unknown`, which must not trigger retry.

Evidence: plan §§9–10; existing VAgent effect correlation is the adapter boundary, not a Vortex
worker callback.

## Semantics and boundaries checked

- The top-level framing is sound: proposed source sketches are explicitly not accepted by current
  compilers; the host supplies owner-minted managed capability cells; no native handle or private ID
  crosses into source.
- The Pending story matches the plan: retain the exact activation/request, park outside VM cycle
  accounting, re-await after notification, and never resubmit. The stated waiter retirement versus
  separate request cancellation is also correct.
- Ownership and cleanup are directionally correct: service/request/lease records survive arena
  rollback and remain charged until backend release; host teardown retires the waiter and
  cancellation is not treated as completion. Keep the owner-only continuation/token separate from
  worker completion messages as the implementation follows this sketch.
- No new Vortex opcode/frontend policy is proposed. These bridges belong in frontend runners and
  generic IoHost/service boundaries; current `ExecutionResult{success,...}` plus synchronous
  `step/run` do not implement Pending. Current VQL, VPL, VProlog, VLace, VIcon, and VAgent APIs
  likewise do not yet expose the described resumable paths, as the example document acknowledges.
- The shared host fixture is an adequate example convention for capabilities: it explicitly says the
  host supplies a managed capability cell, so the examples need not invent a source constructor or
  authority syntax. Operation constructors and tagged match vocabulary are explicitly labeled
  proposed. The missing implementation is therefore not a review blocker by itself.
- Existing VLace `Driver::runIO` is synchronous over `istream`/`ostream`; the plan/example must not
  imply it supplies the async bridge. Existing VIcon `resume()->optional<CellRef>` uses `nullopt`
  for logical exhaustion and cannot represent Parked; the planned result carrier replacement is
  necessary.

## Verdict

**Revise before calling the examples fully lossless and plan-aligned.** The continuation, ownership,
and frontend/core boundary are sound. Make outcome handling total and field-preserving across
examples, and resolve the VLace API mismatch against §10. No target edits made.
