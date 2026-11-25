# Round 1 disposition: Vortex Expert review of M7a (P4/P5 included), with M7-VQL re-checked

Report: [`r1-vortex.md`](r1-vortex.md). Every finding was read against the code at HEAD `94d2c523`
(which includes M7-VQL: `vql/script_runner`, `vortex/file_read_host`, `vquery`) and is confirmed
unless noted. Fix commit:
`fix(vortex): free waiter graphs, safe wake routes, real id audit (M7a review)`.

## M1. A parked await's waiter graph is never freed: CONFIRMED

`ArenaIoCellStore::retireWaiter` only unlinked the request occurrence, so each park left about nine
cells. All three retirement roads (`cancelWaiter`, the service's terminal pass, and `abandon`) go
through that one function, so the fix is there.

- Change: `reapIoWaiter` (`io_cells.cpp`) detaches the request view from the request's clone rank
  (which is also what lets the request itself be reaped), then frees the waiter, its occurrence and
  view, and the three cursor, incarnation and continuation label and value pairs, leaf first. It
  shares `freeOwnedGraph` with `reapIoRequest`, so the 2-degree rule and the clone-rank rule are
  kept in one place. A waiter that has clone views of its own is kept whole.
- Tests: `IoServiceTest.TenThousandAwaitCyclesLeaveTheArenaBoundedToo` (submit, await, half cancel
  and half terminal-pass retire, complete, retire; `liveCells` and `cellCount` equal after 10^4
  cycles) and `IoFrontendRetireTest.ParkedAwaitCyclesLeaveTheArenaBounded` (the same through
  `IoHost`, the VM continuation, `bridge.resume` and `bridge.retire`, 10^4 cycles). With
  `reapIoWaiter` disabled the second fails with 270,070 live cells against 70.
- Price: the earlier "18 live cells per served request" figure was for requests that never park; the
  plan note now says so. The await price is nine more cells while parked and none afterwards.
- MINOR 1 (request retired before `releaseSubmissionCell`): CONFIRMED.
  `IoService::reapRetiredRequest`, `IoHost::reapRetiredRequest`, and `FrontendBridge::release` now
  reaps a request the service no longer indexes once the submission view is gone.
  `FrontendBridge::retire(submission)` releases the sum and then retires the request only if it is
  terminal (`IoHost::retireIfTerminal`); it never cancels a running request. `reapIoRequest` now
  also checks the Request tag, as it is reachable with a caller-supplied handle. Tests:
  `ARequestRetiredBeforeItsSubmissionIsReapedOnRelease` (the live count returns to baseline; before
  the fix it did not) and `RetireNeverCancelsARunningRequest`.
- ScriptRunner (M7-VQL): it did not retire requests, so a long vquery session leaked every finished
  action's request graph. It now retires the replaced action's request when the next action rebinds
  `$submission`, and the last one in its destructor, in both cases only when the request is terminal
  and no parked activation of the same runner awaits it. Because the rows an action returns name its
  outcome cell, the runner must outlive the printing of those rows: `vquery` previously built a
  runner per script, which would have freed the outcome before it was printed, so it now keeps one
  runner for the session (declared after the host so it dies first). Tests:
  `VqlIoScriptTest.ARunnerRetiresEachActionsRequestOnceTheNextReplacesIt` and
  `DestroyingARunnerRetiresItsTerminalRequestOnly`; the existing
  `DestroyingARunnerCancelsItsWaiter NotItsRequest` still holds. `FileScriptWorld` in
  `vql_io_script_test.cpp` keeps one runner too. Known limit: if two activations share one runner, a
  request whose wake is queued but not yet resumed is not counted as awaited; `vquery` runs one
  activation at a time.

## M2. `routeWake` keeps a raw `FrontendRunner*`: CONFIRMED

- Change: `FrontendRunner` owns a `routeLife()` token that expires with the runner (the base class
  destructor), and `IoHost` stores it in the route. `pumpIo` re-checks it immediately before each
  delivery, so a runner that died without `cancelWaiter`, or that another runner's wake destroyed
  earlier in the same pass, is skipped. `unrouteWake(continuation)` and `unrouteRunner(runner)` are
  the explicit API (owner thread); `ScriptRunner`'s destructor calls `unrouteRunner`. The header's
  "must outlive the route" is replaced by this contract. `FrontendRunner` is now non-copyable.
- Tests: `IoFrontendRouteTest.ADeadRunnerIsNeverCalledAndNothingElseIsDropped`,
  `AWakeThatDestroysALaterRunnerInTheSamePassIsSafe`, `UnrouteDropsTheRouteAndLeavesTheParkedAwait`.
- Sanitizer: see the end of this file.

## M3. The no-raw-ids audit cannot detect a leaked id: CONFIRMED

- Change: `auditLanguageValue` now also compares each reachable cell's scalar value (integer, or an
  integral real) with the forbidden ids, besides its text, under any label. The harness passes the
  parked token's `ContinuationId`, waiter cell and request cell as well as the scheduler's tokens.
  Incarnations are deliberately left out: they are small counters that legitimately appear in cells.
- Tests: `checkNoRawIdsInLanguageValues` (so every frontend, including VQL) plants each of the three
  ids as a text cell and as a scalar cell behind the label "benign root" and requires a finding. The
  positive audits still pass, so no legitimate cell collides with those values.

## MINORs

- 2. `wake()` may throw and drop the batch: CONFIRMED. `pumpIo` catches per wake, logs a warning,
     counts it (`IoHost::failedWakes()`), retires that token's VM continuation (the token is Failed
     and nobody can resume it) and delivers the rest. Test:
     `AThrowingWakeIsContainedAndTheBatchIsDelivered`.
- 3. `AlreadyRouted` keeps the first runner and token: CONFIRMED. `routeWake` accepts the same
     runner and activation again (idempotent) and otherwise answers `AlreadyRouted`, which the
     bridge returns as `Failed{IO_TOKEN_STALE}` with a message naming the cause, instead of `Parked`
     carrying the new activation. Test: `AReawaitByAnotherRunnerOrActivationIsRefusedTyped`.
- 4. The reaper never checks owned cells for views: CONFIRMED. `freeOwnedGraph` leaves any reachable
     cell that is the head of a non-empty clone rank (something views it) whole, with its subgraph,
     and only cuts this graph's link to it, so the holder's `cloneMaster` still finds an intact
     cell. Test: `IoFrontendRetireTest.ACellAnotherCellViewsSurvivesTheReap`.

## Not covered (from the report)

`Resumable`/`Abandoned` delivery and the `takeResumedOutcome`-null fallback remain covered only by
the existing harness and bridge tests plus the new route tests; no new case was added for the
fallback. Vlog trail/unify over a freed cell remains guarded only by `CellLinked`, as the report
says.

## No core-file changes

Nothing under `store.hpp`, `ops.hpp`, `compact_op.hpp`, `store_tables.hpp`, `segmented_ops_spool`,
`manifold`, `arena_manifold`, `vortex_core` or `vortex_vm` was edited. Every fix is in `io_cells`,
`arena_io_cell_store`, `io_host`, `io_service`, `frontend_bridge`, `frontend_token`,
`vql/script_runner` and `apps/vquery/main.cpp`. No proposal is recorded.

## Verification

- Focused (`*Frontend*:*Io*:*Conformance*:*Vql*:*Arena*`): 525 passed. Full `xuzz_test`: 4872
  passed; `zigzag_test`: 268 passed; no `E2EBinaryOrchestrationTest` flake occurred. A headless
  `vquery --io-file` read of a real file prints `Complete` and the bytes.
- Sanitizer: `make OBJDIR=build-asan/ sanitize/engine-address` (ASan, UBSan and the integer
  sanitizer), then the frontend, `IoService`, `VqlIo` and `IoCells` suites (137 tests): all passed
  with no AddressSanitizer or LeakSanitizer report. The UBSan integer notes in that run are in
  standard-library headers and `payload_resolver.cpp` (unsigned wraparound in existing hashing), not
  in any file this review touched. The build directory was deleted afterwards.
- Regression strength: with `reapIoWaiter` disabled, `ParkedAwaitCyclesLeaveTheArenaBounded` fails
  (270,070 live cells against 70).
