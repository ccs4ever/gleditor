# Round 1 critique: Vortex Expert (M7-VQL-c code)

Target `dcce529b` (diff `4d132ccf..dcce529b`). Tally: 0 BLOCKING, 1 MAJOR, 3 MINOR.

Lane covered: owner/worker discipline, request/waiter/step lifecycle, same-request resume, abandon
and pin, capture journal ordering, the reapers (`reapIoEndpoint`, `sweepStepGraphs`,
`reapWhenEnded`, `freeFinishedRecords`), VortexCore/VortexVM neutrality. Checked clean: the diff
touches neither `vortex_core` nor `vortex_vm`; a capture's `captureBeginRelation` runs once and a
resume re-enters at the `Await`, so nothing resubmits; no capture step is retried (a refused or
non-`Complete` step ends the capture, `capture_host.cpp:145-153`); the coordinator's phase stays
`AppendRequested` through both durable steps, so `sweepDead` pins a capture that dies at any point
after its append was requested (`capture_coordinator.cpp:746`); the step scaffolding is freed once
the request graph is reaped (the leak tests pass).

## BLOCKING

None.

## MAJOR

### M1. A parked capture whose token the caller dropped is never released when the runner is destroyed, and the host then frees it against a destroyed service

**Problem.** `~ScriptRunner` clears `parked_` (`script_runner.cpp:209`) and unroutes the runner
(`:212`) before it walks `live_` to release captures (`:236-237`). `live_` holds weak pointers. If
the parked script's last strong owner was `parked_` or the host route (the caller dropped its
`ScriptRunResult::Parked` token), clearing them destroys the `Activation`, so `weak.lock()` fails at
`:237`. The `Activation` has no destructor that releases `captures`. The capture's host-side run
(`CaptureHost::runs_`) stays registered, its append request is never orphaned, and the coordinator
never sees the capture cell die, so it is never pinned.

`~FileReadHost` then destroys `service_` before `capture_` (`file_read_host.hpp:221-222`, declared
`capture_` first, so destroyed last; the comment there says this is deliberate so the workers find
the coordinator's adapter alive). `~CaptureHost` runs `freeRun` on every remaining run
(`capture_host.cpp:47-49`), and `freeRun` calls `service_.reapRetiredRequest(...)` (`:284`) through
the `IoService &` the host holds (`capture_host.hpp:174`). That reference is dangling at that point.

**Failure scenario.** A driver parks a script on a capture's append and abandons it without
`expireIfDue` or `resume` (an error or early return in the REPL driver, a dropped token), and the
runner is destroyed. The append later lands in the permascroll through the orphan-less worker
(`DestroyingTheRunnerMidCapture...` shows the append still lands). No pin exists, so a second
capture of the same bytes is not told `Conflict`. A restart's recovery reports the open intent, but
the live session has silently lost the pin the design promises (§11, owner decision Q6). At host
destruction `~CaptureHost` touches the already-destroyed `IoService` (undefined behaviour; no crash
was observed in the release build).

Confirmed with a temporary probe test (built and run, then deleted): park mid-capture exactly as
`DestroyingTheRunnerMidCapturePinsAndResubmitsNothing` does, then drop the token before
`w.runner.reset()`. Result: `liveRuns=1 pinned=0`. The existing tests keep `result` (and so the
`Activation`) alive across `runner.reset()`, which is why they pass.

**Evidence.** `script_runner.cpp:209,212,236-237`; `capture_host.cpp:47-49,284,319`;
`capture_host.hpp:174`; `file_read_host.hpp:216-222`; probe output `liveRuns=1 pinned=0`.

**Fix.** Release captures from state the runner owns, not from the activation's lifetime. Either (a)
keep a runner-level list of every capture handle begun (as `owned_` is for records) and release that
list in `~ScriptRunner`, or (b) walk `live_`/`parked_` and release captures before
`parked_.clear()`. Independently, make `~CaptureHost` not depend on `service_`: have `~FileReadHost`
call a `CaptureHost` "abandon all" while the service is alive, or have `~CaptureHost` free cells
only. Add the dropped-token variant to `VqlIoCaptureAbandonTest`/`expectAbandonedCapturesBounded`.

## MINOR

### m1. An append cancelled before it started is pinned as uncertain

**Problem.** `sweepDead` pins every dead capture past `AppendRequested` unconditionally
(`capture_coordinator.cpp:746`), while `commit` decides uncertainty from
`adapter_.wasExecuted(spec)` (`:1648`). The script-timeout path cancels the awaited step first
(`script_runner.cpp:1708`).

**Failure scenario.** A script times out while its append is still queued behind other work.
`cancelRequest` stops it before the worker runs, so nothing was written and no journal intent
exists, yet the capture's intervals are pinned. A re-run of the same capture is refused `Conflict`
until the user runs `:reconcile`, for a write that provably never happened.

**Evidence.** `capture_coordinator.cpp:746` vs `:1648`; `script_runner.cpp:1708`.

**Fix.** In `sweepDead`, treat `AppendRequested` as uncertain only when `wasExecuted` is true or the
turn is running; otherwise `retire(it, false)`. Needs a regression test with a held scheduler.

### m2. A failed capture step loses its own outcome when the owned copy is refused

**Problem.** `advance` copies a non-`Complete` step outcome with `OwnedOutcome::copy`; if the copy
returns nothing, `ownedStep` stays empty and `fail` uses the coordinator's generic refusal as the
value (`capture_host.cpp:129-134,198-200`).

**Failure scenario.** Under retained-bytes pressure the copy fails; the script reads
`Rejected(InvalidRequest, 7)` instead of the step's real `TimedOut`/`Failed`/`Cancelled`, and cannot
tell an ambiguous (uncertain) abort from a certain one.

**Evidence.** `capture_host.cpp:129-134,152,196-200`.

**Fix.** Have the copy of a payload-less step outcome not depend on the payload budget, or mint a
detached outcome with the tag and `uncertain` flag (`makeDetachedOutcomeCell` exists).

### m3. `CaptureHost::release` ignores a `HasWaiters` refusal from `orphanRequest`

**Problem.** `release` discards the `OrphanResult` (`capture_host.cpp:319`). If a waiter is still
registered the step is neither orphaned nor retired, and `freeRun`'s `reapRetiredRequest` is a no-op
because the service still tracks it. Every current caller cancels the waiter first (timeout,
`~ScriptRunner`), so this is latent: any new caller that releases a parked capture's handle with its
waiter still live pins a service slot until shutdown.

**Evidence.** `capture_host.cpp:319`; compare `ScriptRunner::orphan` handling `HasWaiters`
(`script_runner.cpp:276-300`).

**Fix.** Check the result; on `HasWaiters` cancel the waiter or defer the orphan, and log.

### m4. The documented rule on cancelling a capture step disagrees with the timeout path

**Problem.** `CaptureHost::release` and the header say the awaited step is "never cancelled"
(`capture_host.cpp:316-318`, `capture_host.hpp` `release`), but `expireIfDue` cancels it before
releasing (`script_runner.cpp:1708`; design §11 says "asked to stop" for a timeout and "does not
cancel" for `~ScriptRunner`). The two behaviours are both intended, but the code comment states the
absolute rule and a reader will not find the timeout exception.

**Fix.** State the split in `release`'s comment, or cancel nothing and rely on the orphan alone
(which also removes m1's pin for queued appends).

VERDICT: NOT CONVERGED
