# Round 1 critique: Vortex Expert (Vortex I/O M7a code, P4/P5 included)

Tally: 0 BLOCKING, 3 MAJOR, 4 MINOR. Target `35331b0f..ef74939f`, read from `git show ef74939f:`.

Checked and found sound (no finding): stale-ref safety of a freed or reused slot on every
`denseOf`-routed accessor (a freed slot is an empty run with the freed bit, so no old links or spans
leak to the new tenant); the mark journal for free-then-reuse inside a mark, a free of a pre-mark
cell, a post-mark cell freed and reused, and nested marks (reverse undo restores the LIFO list
exactly); `release()` keyed on the mark's `newestIncarnation`; `compact()` refused under a mark; P5
crash ordering (intent fsync, then `executed_`, then sink append) and the Aborted-then-Append pin
rule.

## BLOCKING

None.

## MAJOR

### M1. A parked await's waiter graph is never freed, so the arena still grows with await traffic

**Problem.** P4 frees only the request graph (`reapIoRequest`) and the submission sum.
`retireWaiter` only detaches the clone view and unlinks the request occurrence; the waiter cell, its
request occurrence and view, and its three `appendField` pairs (`cursor`, `cursorIncarnation`,
`continuation`) are never passed to `freeCell`. **Failure scenario.** A frontend awaits N requests
that park (the normal M7a path). Each park mints about 9 cells (waiter, occurrence, view, 3 labels,
3 values) that no code frees. After 10^6 parked awaits the arena holds about 9x10^6 dead cells, and
`liveCells()` (the `max_live_cells` grant bound) is charged for them, so a long-lived frontend hits
the live-cell cap with no live work. `TenThousandSubmitAndRetireCyclesLeaveTheArenaBounded` never
awaits, so the "bounded by live requests" claim in `p4-p5-implementation.md` is proven only for
requests that do not park. The measured 18 cells/request is also a lower bound for the same reason.
**Evidence.** `arena_io_cell_store.cpp:623-636` (`retireWaiter`, no `freeCell`); `:584-615`
(`mintWaiter` builds the graph); `io_cells.cpp:230-290` and `:674-700` are the only two `freeCell`
sites (grep of `apps/`); `tests/xuzz/io_service_test.cpp:868-930` (no `await`). **Fix.** Free the
waiter graph in `retireWaiter` (leaf first, as `releaseSubmissionCell` does). Add an
await-and-resume cycle to the bounded-arena test, asserting `liveCells()` and `cellCount()`.

### M2. `routeWake` stores a raw `FrontendRunner*` with no way to remove it, so a runner that dies without `cancelWaiter` is a use-after-free

**Problem.** `routes_` holds `&runner`. The only removals are `cancelWaiter` (Removed case),
`collectWake` and `~IoHost`. The header says "the runner must outlive the route" but offers no
`unrouteWake` and no `FrontendRunner` destructor hook. **Failure scenario.** A VQL script runner
parks on request R, then fails or is destroyed through a non-await path (error unwind, script
closed, activation dropped) without calling `cancelWaiter`. R completes, and `pumpIo` runs
`collectWake` and then `wake.route.runner->wake(...)` on the freed object. There is a second path
inside one pass: runner A's `wake()` destroys runner B, whose pending wake is already in the local
`wakes` vector, so B is called after free. The `Abandoned` wake path is the same call. **Evidence.**
`io_host.cpp:209-224` (`routeWake`), `:237-244` (`collectWake`), `:296-299` (delivery loop);
`io_host.hpp` ("must outlive the route"). The harness `World`/driver
(`io_frontend_conformance.hpp:185-201`) never destroys a driver while parked, so this is untested.
**Fix.** Add `IoHost::unrouteWake(token)` (owner thread), required in every runner's teardown, and
have the delivery loop re-check the route (or hold a `weak_ptr`/generation) just before calling. Add
a conformance case: park, destroy the driver, complete, pump.

### M3. The conformance harness's "no raw ids" check cannot detect a leaked continuation, waiter or request id

**Problem.** The "forbidden decimals" are only the scheduler's private tokens (`0xC0FFEE00+`), which
no code path writes into a cell. The ids the contract names (`ContinuationId`, waiter ref,
`RequestId`, all available in `parkedToken`) are never passed, so the decimal branch of
`auditLanguageValue` is never exercised against a real id. The label branch only catches cells whose
text contains "continuation", "waiter" and so on. **Failure scenario.** A frontend binds a cell with
text equal to `std::to_string(token.continuation)` and a benign label. `NoRawIdsInLanguageValues`
passes, although plan §10 says no raw id is visible. **Evidence.**
`io_frontend_conformance.hpp:362-368` (`forbidden` built from `entries[].command.token` only) and
`:370-372` (the self-check proves only the label branch); `frontend_bridge.cpp:236-243`. **Fix.**
Add `parkedToken.continuation`, the waiter and request incarnations, the cell ids and the cursor to
`forbidden`. Add a negative test that plants such a cell and expects a finding.

## MINOR

1. **Submission retire order leaks the request graph, silently.** If the request is retired before
   `releaseSubmissionCell`, `reapIoRequest` returns early on the clone view, and nothing re-reaps
   after the view leaves (`io_cells.cpp:240-246`, `:674-700`; the header comment "becomes reapable
   again" names no caller). `FrontendBridge` has no `retire(submission)` that does release then
   retire, and the harness never retires. Add that method and a test.
1. **`wake()` may throw and drop the rest of the batch.** `FrontendRunner::wake` is not `noexcept`.
   An exception in `io_host.cpp:296-299` destroys `wakes`, so later runners' parked entries are
   erased, their VM cursors resumed, and nobody is told. Catch and log per wake, or make it
   `noexcept`.
1. **`AlreadyRouted` keeps the first runner and token.** `frontend_bridge.cpp:175-183` returns
   `Parked{token}` carrying the new activation, but the host will wake the first runner with the
   first activation. A re-await with a different activation or runner is silently misrouted. Compare
   the runner and return a typed refusal.
1. **The reaper never checks owned cells for views.** The `owned` loop calls `detachFromRank` on
   every owned cell, so an outcome that someone holds through a clone view loses its head, and that
   holder's `cloneMaster` returns a malformed graph instead of a stale handle
   (`io_cells.cpp:262-270`). Refuse or keep the cell when its clone rank is non-empty.

## Not covered

- `Resumable`/`Abandoned` wake delivery and re-parking inside `wake()` are untested; the fixture's
  `wake` only records. The `takeResumedOutcome`-null fallback path is untested.
- `freeCell` is called only by the I/O reaper, so Vlog trail/unify over a freed cell is guarded only
  by `CellLinked` (a bound variable cannot be freed).

VERDICT: NOT CONVERGED
