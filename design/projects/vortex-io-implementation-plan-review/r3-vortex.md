# R3 Vortex runtime review

- Reviewed target: `design/vortex-io-implementation-plan.md`
- Target SHA-256: `c1032a0507b45c7352758d0f66ac03eb328b3d080d0a60379b8d3f34e78e1891`
- Scope: owner-side waiter mint/retire and completion races, scheduler release
  acknowledgement, and preservation of generic per-cursor execution.
- Result: **0 BLOCKING, 0 MAJOR, 0 MINOR — CONVERGED.** The added lifecycle
  details close the previously implicit registration/cleanup path and preserve
  cursor-scoped VM semantics.

## Waiter lifecycle and race review

`OwnerWaiterRegistration` carries cursor ref, cursor incarnation and
continuation. The host supplies it; the owner-only `IoCellStore` mints a
transient waiter only when `IoService::await` sees the request still pending.
Await serializes with owner `pump`: a completion candidate is not terminal
until pump publication, so an earlier registration is woken by pump; a request
already made terminal yields `Ready` without a waiter. This prevents the
completion-before-registration lost wakeup. Duplicate request/cursor/
continuation registration is idempotent.

The waiter is retired after resume, idempotently. Waiter cancellation only
removes that waiter; request cancellation is separate. Frontend activation
cancellation removes its active waiter, while a waiter may be shared neither
across cursor registrations nor confused with the request's lifetime. The
host pump routes notification to exactly one `(cursor incarnation,
continuation)`; multiple waiters on a request and multiple parked awaits per
cursor remain supported. These details preserve the generic per-cursor
`VortexVM::step/run/resumeAwait` contract.

## Worker release acknowledgement review

Terminal result and backend release acknowledgement are distinct messages.
The sink holds whichever arrives first and completes the reserved terminal
record only when both are present. This keeps buffers and request metadata
charged until native handles, buffers, kernel registrations and other users
are released. A queued cancellation has no backend user, so the scheduler
provides both a cancelled candidate and release acknowledgement without
starting work. A running cancellation does not synthesize terminal completion
before cleanup; abnormal worker exit becomes `Unknown` only after supervisor
cleanup, and unconfirmed cleanup remains charged and backpressures admission.
Thus late cleanup cannot race into reclaimed worker state or cause silent
resubmission.

## Source and limits

Checked the current per-cursor API against `vortex_vm.hpp` and arena mark/ref
semantics against `arena_manifold.hpp`; no source conflict found. This was a
design review only; no runtime tests or source edits were made.
