# Round 2 critique: System Architect (vortex-io-implementation-plan.md)

Reviewed target SHA-256: `ec5d3722af8be1d433bb9e535141184da7c10ca4df4ee6afb050c7d58112ac17`.

Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Prior R1 findings

- **R1.1 (Open had no asynchronous lifecycle): RESOLVED.** `admitOpen` now creates an ordinary
  request without publishing an endpoint, and successful owner publication creates the endpoint and
  capability; failure/cancellation waits for backend acknowledgement (§4).
- **R1.2 (IoBackend and IoLens had disconnected execution paths): RESOLVED.** `IoAdapter` is the
  single registered dispatch interface selected by endpoint kind and operation. The service owns
  admission, scheduling, cancellation, and completion routing (§§4, 6).

## MAJOR

### R2.1 — Scheduler/worker completion and cancellation contract is still unspecified

The plan names `IoScheduler::schedule(BackendCommand, IoAdapter&, CompletionSink&)` and requires
the worker to emit a `NativeCompletion`, but `CompletionSink` and `BackendContext` are only forward
declarations. `schedule` returns `void`, and the plan does not define whether it is nonblocking and
infallible after service admission, how bounded queue capacity is reserved, how cancellation state
reaches a running adapter, or how the sink safely accepts progress and exactly one terminal
completion from worker threads. Those details are essential to uphold the stated invariants that
admitted work is bounded, completion cannot be lost, cancellation retains native leases until
acknowledgement, and no owner thread blocks. An implementer still has to invent the scheduler's
central lifecycle contract.

**Required fix:** Specify the worker-facing types and their contract: admission reserves scheduler
capacity before an operation becomes admitted; `schedule` is nonblocking with a defined
post-reservation failure rule; the worker receives an explicit cancellation view/token; and a
thread-safe completion sink accepts bounded/coalescible progress plus exactly one terminal result,
retaining payload leases and delivering cleanup acknowledgement to the owner. State how a scheduler
failure after admission becomes the request's terminal result without dropping its identity.

Evidence: plan §§1, 4 (`IoScheduler`, `BackendCommand`, `NativeCompletion`), 8.1, 13 M1; redesign
§§1, 6.1, 6.3.

## Positive architecture checks

- Open and close use the common request/Pending path, resolving R1.1.
- There is one service-owned adapter dispatch path, resolving R1.2.
- Core/VM remain language-neutral; frontend syntax/effect rules remain frontend-owned.
- Bounded payload/chunk accounting, configuration snapshots, actor durable admission/recovery,
  capture ordering and overlap identity, and milestone dependencies remain coherent in this revision.

VERDICT: REVISION REQUIRED (one major scheduler lifecycle contract gap).
