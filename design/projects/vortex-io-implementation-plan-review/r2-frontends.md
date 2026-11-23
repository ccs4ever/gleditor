# R2 frontend language review

- Reviewed target: `design/vortex-io-implementation-plan.md`
- Target SHA-256: `ec5d3722af8be1d433bb9e535141184da7c10ca4df4ee6afb050c7d58112ac17`
- Scope: R1 resolutions and callable/lowering contracts for VQL, VPL,
  VProlog, VLace, VIcon, and VAgent.
- Result: **1 MAJOR, 1 MINOR, 0 BLOCKING — REVISION REQUIRED.** The revision
  resolves the language-visible admission sum, keeps native identifiers out
  of source values, restores cursor-scoped VM execution, and substantially
  improves all six bridge paths. A shared single-request token still conflicts
  with VPL's multi-position await, and the Prolog call trace awaits a sum even
  though the common contract requires awaiting only admitted requests.

## Findings

### F1 — MAJOR: Shared `FrontendToken` cannot represent VPL's multiple pending requests

Section 10 defines one `FrontendToken` as retaining the frontend activation
plus "the managed request cell". The VPL row simultaneously defines
`await¨ R`, a shape-preserving result for every input position, and
`Parked{activation, requestPositions}` containing potentially several pending
positions. Section 5 correctly allows multiple waiter registrations and
parked awaits, each with a distinct continuation; section 3 also defines a
Waiter cell per awaiter. But no contract joins these pieces for one VPL
activation: does the parked token own one waiter per pending position, does
the runner wake on the first completion or only when all positions are ready,
and what happens when an admission refusal or terminal slot coexists with
several pending slots? The singular token contract cannot own the VPL set.

This leaves both wakeup and resume behavior implementation-defined. A
completion can arrive out of order, and VPL must preserve the input positions
while waking only the owning activation; neither `requestPositions` nor the
shared token describes waiter-to-position correspondence or re-awaiting
ready positions.

**Concrete fix:** define `FrontendToken` with a collection of pending entries
`{position, RequestCell, WaiterCell}` (or a VPL-owned aggregate waiter whose
child registrations are explicit). Specify the policy: wake the parked
activation when any pending position completes; on resume, re-await only
still-pending positions and read terminal outcomes from ready positions, then
park again if needed. Refusals and terminal slots remain in place and do not
have waiters. State cancellation ownership per position and ensure a single
VPL activation can be canceled without canceling sibling requests unless
explicitly requested. Other frontends may use the same collection with one
entry.

Evidence: plan §3 Cell value table defines one Waiter per request/continuation;
§5 says one request may have multiple waiters and one cursor multiple parked
awaits; §10's shared token is singular while its VPL result is position-wise.
VPL `Executor::execute/run` currently returns a final `ExecutionResult` with a
`VplView` (`apps/common/xanadu/vpl/execution.hpp`), so the new parked result
must explicitly carry the exact view and per-position registration state.

### F2 — MINOR: Prolog example passes a submission sum to `io_await`

Section 10 establishes `await` accepts only `Admitted.request`, with
`Refused` preserved as a value. The VProlog row's path is
`io_submit(Cap, read(Off,Max), Submission), io_await(Submission, Outcome)`.
Although the preceding sentence says `Submission` unifies with
`admitted(Req)` or `refused(OutcomeCell)` and only admitted branches call
await, the displayed goal passes `Submission` itself to `io_await/2`. That
contradicts the explicit shared type rule and leaves the no-failure mapping
unclear in Prolog's goal language.

**Concrete fix:** either add a Prolog sum-aware `io_observe/2` wrapper that
passes `refused(OutcomeCell)` through and awaits only `admitted(Req)`, or show
the explicit branch with refusal returned as an ordinary bound term. Do not
make `io_await/2` accept native RequestId or turn refusal into predicate
failure.

Evidence: §3.2 and §10 common sum contract; the row below it; Prolog query
terms and goals are compiled through Vortex/Vlog (`apps/common/xanadu/vprolog/compiler.hpp`).

## R1 resolution audit

| R1 finding | R2 disposition | Evidence / remaining note |
| --- | --- | --- |
| Common managed submit/refusal value unclear | **Resolved** | §3.2 and §10 define `Admitted{request}` / `Refused{outcome}` cells and require pattern matching before await. The prose explicitly forbids missing-cell, failure, no-row and dropped-slot interpretations. |
| Native RequestId could cross language boundary | **Resolved** | §3.2, §5 and §10 mark RequestId/WaiterId/ContinuationId private; `PendingAwait` contains only continuation and waiter. |
| Cursor identity and completion routing | **Resolved** | §5 preserves `step(cursor)`/`run(cursor)` and routes waiter completion to exactly one cursor/continuation; it permits multiple waiters and parked awaits. |
| VQL grammar/continuation vague | **Mostly resolved** | §10 specifies a script-only EffectClause/binding/match change, row expressions remain excluded, and script activation gets a Parked result. This is a planned grammar extension, explicitly gateable, not a current source form. `VQL_IO_QUERY_CONTEXT` makes no per-row fanout unsupported. |
| VPL shape/refusal/Pending vague | **Partially resolved** | Shape sum and refusal pass-through are clear; pending-position ownership and wake policy remain F1. |
| Prolog typed refusal and suspension vague | **Partially resolved** | Top-level effect context, typed binding, Parked compiled query/substitution/trail and diagnostic are specified. The example's direct await of Submission remains F2. |
| VLace perform/laziness vague | **Resolved for first bridge** | §10 names a pure `readBytes` action constructor, `runIOAsync`, a distinct parked `IOContinuation`, same-request resume, and `VLACE_ASYNC_UNAVAILABLE` without blocking fallback. Exact source grammar is expressly planned; keeping the node opaque avoids thunk-driven effect admission. |
| VAgent form/source mapping vague | **Resolved enough for implementation planning** | §10 now uses a capacity effect signature and plan effect-call form, retains the typed outbox and post-commit durable admission, and specifies managed outcomes in later events plus actor-suspension refusal. Source model is corroborated by `vagent/parser.cpp`, `checker.cpp`, `executor.cpp`, and `adapter.hpp`. |
| Direct Vortex row count/table formatting | **Resolved** | Explicitly labeled shared-host baseline, outside the six bridge count; result is no longer split by a Markdown pipe. |

## Source-grounded bridge assessment

- **VQL:** The table now aligns with its existing `ExecutionBlock` /
  `ActionClause` / `EffectClause` structure and script parser; it limits the new
  match binding to script grammar and keeps path/query expressions and row
  traversal pure. This is a real grammar extension, but the target form and
  rejection diagnostic are clear enough to implement in M7.
- **VPL:** `submit¨` reuses the existing each-adverb lowering and carries the
  sum at original shape positions. Equal-shape refusal precedes admission and
  completion order cannot reorder values. F1 is the outstanding multi-pending
  continuation detail.
- **VProlog/Vlog:** Tagged terms fit the AST's `Compound`/`Var` values. The
  query driver owns continuation restoration of compiled query, VM cursor,
  substitution and trail; speculative effects are prohibited with a driver
  diagnostic rather than represented as ordinary predicate failure. Correct
  only the call trace under F2.
- **VLace:** The plan correctly keeps lazy action construction inert and moves
  admission to a new asynchronous driver entry. Existing `IO.*` constructors
  are opaque and synchronous today (`vlace/lower_io.cpp`), so M7 must gate the
  whole path until `runIOAsync` exists; the plan explicitly says this.
- **VIcon:** The driver result distinguishes Yielded, Parked, Failed and
  Finished; the old `optional<CellRef>`/Icon failure ambiguity does not swallow
  Pending. `write(value)` remains a yield, and stdout is a separate explicit
  sink. This matches `vicon/driver.hpp`'s current contract.
- **VAgent:** Current actor effect calls already stage outbox entries and
  correlate later `EffectDone`/`EffectFailed` events; the proposed row keeps
  that policy and avoids moving mutable `NativeBody(WorkerContext&)` to an I/O
  thread. The durable attempt gate before service admission is the required
  actor boundary.

## Tally and verdict

| Severity | Count |
| --- | ---: |
| Blocking | 0 |
| Major | 1 |
| Minor | 1 |

**Verdict: REVISION REQUIRED.** R1's central interface concerns are resolved.
Specify set-valued frontend wait ownership and correct the Prolog sum/await
example before treating the six frontend traces as fully implementable.

## Review limits

Read the frozen plan at the recorded SHA and the frontend parser/AST/executor
boundaries named above. No tests were run and no implementation files were
changed.
