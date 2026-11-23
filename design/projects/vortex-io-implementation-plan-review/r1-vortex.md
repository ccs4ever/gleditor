# R1 Vortex runtime review

- Reviewed target: `design/vortex-io-implementation-plan.md`
- Target SHA-256: `f619801374ad6b81638e719014243cbccc354fdfee5ca9c1579b43b93bc80093`
- Scope: VortexCore/VM execution stepping, pending/resume and trail semantics,
  identity safety, and source-integrable runtime interfaces.
- Result: **2 MAJOR, 0 BLOCKING, 0 MINOR — REVISION REQUIRED.** The plan makes
  sound ownership and no-resubmit decisions, but its generic VM entry points
  do not preserve the current cursor model, and allocation-incarnation
  validation has no defined route from a program `CellRef` to its token.

## Findings

### V1 — MAJOR: generic execution signatures drop cursor identity and do not define wakeup routing

Section 5 replaces the existing `step(CellRef cursor)` and
`run(CellRef cursor, maxCycles, stopToken)` with `step(cycleBudget)` and
`run(cycleBudget)`, while `resumeAwait(ContinuationId, outcome)` takes neither a
cursor nor a completion/waiter token. The compiled VM is explicitly
multi-cursor: `ChoicePoint` records an optional cursor, `activeCursors()` feeds
`stepScheduler()`, and the current execution entry points all carry the cursor
(`apps/common/xanadu/vortex/vortex_vm.hpp:520-543, 672-676`). A continuation ID
alone is not specified as a VM-global key, nor does the plan say how a service
completion wakes the correct cursor or handles multiple awaits on one request.
This leaves a central API underspecified and risks collapsing independent
process cursors into one parked state.

**Concrete fix:** preserve cursor in each execution operation, e.g.
`step(CellRef cursor)`, `run(CellRef cursor, size_t maxCycles, stop_token)`, and
`resumeAwait(CellRef cursor, ContinuationId, IoOutcome)` (or define a globally
unique continuation key that provably resolves to exactly one cursor). Freeze
one wakeup sequence: owner pump receives the service completion/wait token,
resolves it to the parked cursor and continuation, then resumes that cursor
without re-running Submit. Define whether `step` means one opcode or a bounded
run; the existing `step` is one cursor opcode, while `run` owns cycle budgeting.

### V2 — MAJOR: `AllocationIncarnation` is required but no API creates or recovers it for VM operands

The plan requires wrapper validation and says the await opcode validates the
request cell's allocation incarnation (sections 3.2, 4, and 5). The only
specified representation is an owner-only `RequestCell{CellRef,
AllocationIncarnation}` wrapper. The program-facing handle and VM opcode are
still a `CellRef`, and there is no specified arena allocation return token,
incarnation lookup, side table, or cell-value representation connecting that
`CellRef` to the wrapper. Thus the critical safety check cannot be implemented
from the proposed opcode input. This is material because current `Mark` stores
allocation counts and `release()` truncates minted cells; a later allocation
may reuse the same dense cell number (`arena_manifold.hpp:203-229, 840-849`).
Mark serial is per-choice-point and does not identify an individual allocation.

**Concrete fix:** settle the authoritative handle path. For example, make
arena minting assign an `AllocationIncarnation` and provide an owner-only
`incarnationOf(CellRef)` / `matches(CellRef, AllocationIncarnation)` contract,
with the VM/service registry retaining the expected incarnation for each
managed IO cell; or make the managed program handle explicitly resolve to a
`RequestCell` sidecar entry whose generation is checked before any use. State
how identity behaves for base-manifold/proxy cells, rollback, cut, overwrite,
and arena destruction. Keep the incarnation opaque and never parse it from
cell text.

## Additional consistency note

`RequestId` is declared native-private in section 4 (`IoService::submit` result
and §4 text), but section 5 includes it in `PendingAwait`, described as the VM
host result. Clarify whether `RequestId` is internal-only; the clean boundary
is to keep it inside the service and route wakeup through the waiter/continuation
registration. This can be resolved alongside V1 without exposing native IDs
to a language bridge.

## Evidence and limits

Reviewed the plan at the recorded hash, v0.11 source contract, `vortex_vm.hpp`,
`arena_manifold.hpp`, and the relevant `VortexVM::run` implementation. The
source confirms `step` is cursor-scoped and `run` checks a stop token as an
execution cancellation; existing behavior should not be repurposed as an IO
Pending result. This review did not run tests or change runtime sources.
