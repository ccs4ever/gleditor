# R2 review: Vortex execution and continuation

Target verified: `design/vortex-io-frontend-examples.md`, SHA-256
`ce9b9815f379d706588813ea6f4d17b2fc383b6d0408deeae6190eb2f3f4e594`.

Reference: `design/vortex-io-implementation-plan.md` §§1–3, 9–10; current Vortex VM and frontend
runner APIs. This is a design review of the proposed bridges, not an implementation verification.

## Findings

No blocking or major findings.

## Prior findings rechecked

- **Outcome preservation: resolved.** Every read example now has a total match over its legal
  terminal variants and retains the full managed outcome. Payload reads are guarded by
  payload-bearing tags/presence; progress, uncertainty, gaps, oversize disposition, and generation
  fields are preserved. Submission `Refused` remains distinct from terminal `IoOutcome`. VPL states
  the deterministic left-to-right bounded admission rule and retains refusals in their original
  positions.
- **VLace alignment: resolved.** The example restores the plan's `readBytes` / `runIOAsync` lazy
  action surface and expressly defines its lowering to the shared capability, Submit/Await, request,
  and outcome protocol. `Done(IOValue)` and `Parked{IOContinuation, RequestCell}` are explained, and
  Pending resumes the same continuation/request without performing the action again. It is clearly
  identified as a companion-local lowering contract, not a currently implemented method.
- **VAgent event mapping: resolved.** Admitted operations return `EffectDone` with the original full
  terminal outcome regardless of success or failure; pre-admission refusal is a typed `EffectFailed`
  value. Actor deliberation stages an intent and never suspends; durable journal acknowledgement
  precedes service admission. `Unknown` is not automatically retried.

## Semantics and boundaries checked

- Pending remains runner state, not an outcome or language value. Each bridge parks the exact
  activation and request and re-awaits it after wakeup. Fresh execution of an effect site is
  explicitly new work.
- Ownership is coherent across rollback and teardown: capability authority is host supplied; request
  and lease ownership survives arena rollback and lasts until backend release; waiter retirement is
  separate from request cancellation; cancellation does not imply backend stop. No solver reference
  or mutable worker context crosses to I/O workers.
- The plan's architecture boundary is maintained: the examples add no frontend opcode or I/O policy
  to VortexCore/VortexVM. Current VM `ExecutionResult` and synchronous `step/run` do not yet
  implement Pending, and the current frontend APIs remain synchronous; the document identifies these
  as implementation work.
- The source fixtures are sufficiently explicit for proposed sketches. Host-injected managed
  capabilities and operation/outcome pattern syntax are labeled as proposed, so the examples need
  not invent public authority constructors. Capture remains host-only and explicit.
- VIcon distinguishes current `nullopt` exhaustion from the proposed `Parked` driver result. VProlog
  presents the result directly rather than routing it through a fallible user predicate. VPL
  preflights shape before admission and preserves per-position results.

## Verdict

**Accept for the Vortex execution/continuation scope.** The revised examples now preserve typed
outcomes and fields, align VLace with the plan's target surface, and maintain the
owner/service/worker boundary. No target edits made.
