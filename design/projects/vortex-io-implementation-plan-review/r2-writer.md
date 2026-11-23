# R2 — Technical writer review

Target: `design/vortex-io-implementation-plan.md`  
Target SHA-256: `ec5d3722af8be1d433bb9e535141184da7c10ca4df4ee6afb050c7d58112ac17`  
Review scope: R1 resolution, cross-layer names/types/examples, normative versus illustrative status, and milestone consistency.  
Verdict: **RETURN FOR REVISION**

## R1 resolution

Both R1 findings are resolved in this version. `EndOfStream` is now the sole EOF spelling in the API and examples. The Direct Vortex table row is explicitly labeled as the shared-host baseline outside the six bridges; its result separator no longer splits the Markdown table, and M7 separately names direct-host conformance.

## Finding

### MAJOR — WaiterCell creation and direction through Await are still unspecified

**Evidence:** §4 declares `AwaitResult await(RequestCell, WaiterCell)`, which takes a `WaiterCell` argument, while the result contract says `Pending{ContinuationId, WaiterCell}` returns a waiter. No service operation or host API mints/registers the input waiter. In §5, the VM creates the continuation and waiter identity when parking, but `IoHost::awaitCell(RequestCell, cursor, frontendContinuation)` passes no `WaiterCell` to the service. The general invariant likewise describes `Await` as producing the waiter. Thus the service, VM, and host signatures disagree about whether the waiter exists before Await or is created by it.

**Why it matters:** Implementers cannot define the registration and wakeup sequence without inventing an API. The waiter is the service-to-owner wake key, so this affects race-free registration if completion arrives between the initial poll and park, and cleanup when the owner cancels a waiter.

**Required fix:** Choose one direction. Prefer `IoService::await(RequestCell, OwnerWaiterRegistration)` returning `Ready` or `Pending{ContinuationId, WaiterCell}`, with the host minting/registering the waiter atomically with the pending check; or define an explicit owner-only waiter creation/registration API and make the signatures consistent. State the completion-before-registration behavior and waiter retirement rule.

## Cross-review note

The frontend reviewer separately reported a related multi-request issue: VPL's parked vector await carries multiple request positions, while the general `FrontendToken` description names one request cell. That report should specify a request-position/waiter set and wake policy; I have not duplicated it as a second finding here.

## Positive checks

- The plan now distinguishes `OutcomeTag::Rejected` in a refusal cell from admitted terminal `IoOutcome` variants.
- Open, cancellation, adapter dispatch, and capture now use the common service request lifecycle.
- The frontend section distinguishes admission refusal, execution `Pending`, and terminal outcomes, and clearly names the direct-host conformance path.
- M0–M8 have dependencies and exit evidence; M7 now explicitly covers six language bridges plus direct-host conformance.

## Tally

- Blocking: 0
- Major: 1
- Minor: 0
- Nit: 0

I verified the target SHA-256 above. I did not modify the plan or runtime sources and did not run tests.
