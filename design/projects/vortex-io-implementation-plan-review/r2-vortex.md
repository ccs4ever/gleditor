# R2 Vortex runtime review

- Reviewed target: `design/vortex-io-implementation-plan.md`
- Target SHA-256: `f11616b8ff83847257ac842c4a35374a1856e211a8c7db2b76f9c850b237647d`
- Scope: verify R1 findings and review the revised execution, wakeup, identity,
  ownership, trail, and durability interfaces against the Vortex source.
- Result: **0 BLOCKING, 0 MAJOR, 0 MINOR — CONVERGED.** R1's cursor routing
  and incarnation gaps are resolved with concrete interfaces and ownership.

## R1 finding verification

- **V1 resolved.** Section 5 retains the VM's per-cursor signatures:
  `step(cursor)`, `run(cursor, maxCycles, stopToken)`, and
  `resumeAwait(cursor, continuation, outcome)`. It defines owner pump routing
  through each waiter's one `(cursor, continuation)` registration, supports
  multiple waiters and multiple parked awaits, and makes Pending separate
  from current stop/cut cancellation. This matches the existing
  `VortexVM::step(CellRef)`, `run(CellRef, ...)`, active cursor list, and
  scheduler (`apps/common/xanadu/vortex/vortex_vm.hpp:520-543, 672-676`).
- **V2 resolved.** The plan specifies `MintedCell{ref, incarnation}`,
  `incarnationOf(ref)`, `matches(ref, incarnation)`, non-reuse after rollback,
  origin identity for base/proxy refs, and destruction/overwrite/lease rules.
  This directly addresses the dense-index reuse caused by arena mark release
  (`apps/common/xanadu/zigzag/arena_manifold.hpp:203-229, 840-849`).
- **RequestId privacy resolved.** VM Pending results now contain only
  `ContinuationId` and `WaiterCell`; service-private request tokens stay in
  `BackendCommand`/`NativeCompletion`, outside language values.

## Cross-interface review

The API layers now agree on ownership: workers receive only immutable,
cell-free commands and return private-token completions; owner publication
validates request/source generations and cell incarnations; managed outcome
cells are then the language-visible result. `IoService::await` produces
`Ready{OutcomeCell}` or `Pending{ContinuationId, WaiterCell}`, and the generic
host-to-runner path resumes the same cursor/activation without re-running
Submit. Backtracking rematerializes a retained request result into ordinary
trailed arena state, while request records survive arena marks for semantic or
backend owners. Waiter cancellation and request cancellation remain distinct.

`VortexCore` stays free of service scheduling and frontend policy. The plan
preserves `step` as one opcode and cycle budgeting in `run`, and explicitly
keeps Pending apart from the current stop-token-as-cut behavior. It also places
the VM/API integration milestones after the service lifecycle and arena
incarnation work. The source confirms the inspected compiled runtime is
`VortexCore` + `VortexVM`; no legacy VM coupling is required by the proposal.

## Note

`resumeAwait` accepts a native `IoOutcome` value while program code observes an
`OutcomeCell`. The plan places resume on the owner path and describes
owner-side materialization, so this is a valid internal handoff. During
implementation, keep the method owner-only and validate the supplied
continuation against the waiter's registered cursor before publishing the
outcome cell; no additional design finding remains.

No runtime tests were run and no source implementation was changed for this
review.
