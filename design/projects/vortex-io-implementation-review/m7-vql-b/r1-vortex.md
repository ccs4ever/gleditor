# Round 1 critique: Vortex Expert (M7-VQL-b code, 27768e54..bda0e78b)

Tally: BLOCKING 0, MAJOR 3, MINOR 4. Read-only review, tests not executed.

IoService deadline enforcement: sound. The race analysis is below under MINOR notes. Every
`OpenSpec::deadline` reader is updated (only `io_service.cpp:160` reads it). Lane 1 has no BLOCKING
or MAJOR finding.

## BLOCKING

None.

## MAJOR

### M1. `expireIfDue` can orphan the awaited request instead of stopping it, and no test proves the stop

**Problem.** `expireIfDue` resets `activation->parked` (destroying the `ProgramRun`, whose `close()`
runs `abortScopedChoices` and so `releaseChoiceMark` and `rolledBack`) before `abandonAll`. For a
Submit under a live choice point, `rolledBack` calls `orphan(request)` and erases the entry from
`activation->admitted`. `orphan` never cancels. By the time `abandonAll` runs, the entry is gone, so
`retireRequest` (the only call that asks the backend to stop) is never made.

**Failure scenario.** The script `read ... ; await` parks on a slow read with no `within`. The
script deadline passes and `expireIfDue` runs. The waiter and route are dropped and the request is
orphaned, but its backend keeps running to completion, holding its descriptor and budget slot. A
blocking backend means it never ends. The diagnostic says "the request it awaited was asked to
stop", which is false. The plan note ("awaited request asked to stop (`retireRequest`)") is likewise
false on this path.

**Test gap.** `AScriptPastItsOverallDeadlineIsAbandonedStably` and
`TheDriverReportsTheTimeoutAndLeavesNoBudgetBehind` assert only the code, `position` and
`parkedCount`. The second test's name claims "no budget left behind" and asserts nothing about it.
Neither checks that the request was cancelled, retired or released.

**Evidence.**

- `apps/common/xanadu/vql/script_runner.cpp:849-863` (`parked.reset()` before `abandonAll`)
- `apps/common/xanadu/vql/script_runner.cpp:311-338` (`rolledBack` orphans and erases)
- `apps/common/xanadu/vortex/vortex_vm.cpp:1552-1562` (`releaseChoiceMark` notifies)
- `tests/xuzz/vql_io_script_test.cpp:1399-1440`

**Fix.** In `expireIfDue`, call `abandonAll(activation->admitted)` (and `closeOpened`) before
`activation->parked.reset()`. Make the tests assert the awaited request ends `Cancelled`, the
service's `requests_` and budget return to zero, and the endpoint is closed.

### M2. `closeOpened` is refused at budget exhaustion and the descriptor leaks silently

**Problem.** `closeOpened` runs in `finish`, `endActivation` and `expireIfDue` before the script's
own terminal requests are retired. `IoService::admit` refuses with `BudgetExceeded` when
`requests_.size() >= maxRequests`, and terminal-but-unretired requests count. A script that
saturated the budget has its close admitted as a `Refused` arm value. `closeOpened` only logs when
`closeCapability` itself returns `unexpected`. A refused close goes into `closing_` with no request
and is retired. There is no retry and no log, so the capability's endpoint is never closed.

**Failure scenario.** The script opens file F, then issues reads until admission returns the
`BudgetExceeded` arm, or exactly fills `maxRequests`. At script end the close is refused and F's
descriptor stays open until service shutdown. This breaks the stated "every ending closes" guarantee
exactly when resources are tight.

**Evidence.**

- `apps/common/xanadu/vql/script_runner.cpp:255-282` (`closeOpened`)
- `apps/common/xanadu/vql/script_runner.cpp:733-741` (`finish`)
- `apps/common/xanadu/vortex/io_service.cpp:211-213` (budget check includes the close)

**Fix.**

- Retire the activation's terminal entries before admitting closes.
- Treat a `Refused` close arm as a failure: log it, and keep the capability for retry on the next
  `reapClosing`.
- A close for an owned endpoint arguably should not be budget-refusable. If so, say so in the plan.

### M3. `~ScriptRunner` forgets running requests and close requests, a permanent leak while the service outlives it

**Problem.** The destructor calls `retireAll(finished_)`, `retireAll(activation->admitted)` and
`reapClosing()` with `force=false`. `retire(..., false)` returns false for a non-terminal request,
and the entry is then dropped with the runner. The closes `~ScriptRunner` has just admitted through
`closeOpened` are never terminal at that instant (they queue behind in-flight work and need a pump).
So every opened capability leaves one close request in `requests_` forever.

Nothing ever retires it. A terminal state stays until `retireRequest` or `orphanRequest`, and
`orphanRequest` is only reached via `force`. Running detached reads leak the same way.

**Failure scenario.** An embedder creates and destroys a runner per session on a long-lived
`IoService`. Each destroyed runner that opened a file, or has a detached read in flight, strands at
least one `requests_` entry plus its arena cells and budget slot. After `maxRequests` such leaks the
service refuses every admission with `BudgetExceeded`.

The plan lists "destroying a runner never cancels a running request" as a deferral, but the actual
behaviour is worse: the request is never retired either. The header says "forgotten at its own end".
`retire` without `force` does not do that.

**Evidence.**

- `apps/common/xanadu/vql/script_runner.cpp:160-176` (dtor)
- `apps/common/xanadu/vql/script_runner.cpp:218-235` (`retire`: only `force` orphans)
- `apps/common/xanadu/vortex/io_service.cpp:637-660` (`orphanRequest` is the forget path)
- `design/vortex-io-implementation-plan.md`, "Running detached reads" note

**Fix.** The destructor passes `force=true` to all three `retireAll` and `reapClosing` calls, so
running requests are orphaned (forgotten at their end, still not cancelled). That matches the
documented non-cancel deferral and removes the leak. Add a test that destroys a runner mid-read and
asserts the service's request count returns to zero after a pump.

## MINOR

1. **Deadline race: confirmed sound.** For reference:

   - The `candidate` and `cancelRequested` checks in `expireDue` skip anything with a result in
     hand.
   - `stopRequest` returns `AlreadyTerminal` if the scheduler job is already `Completed`.
   - A running stop only sets a stop token, so `expired` is applied to a `Cancelled` outcome only.
     `Complete` and `Failed` keep their value.
   - The synthetic path owns no worker or payload.
   - A duplicate candidate releases its lease (`io_service.cpp:766-773`).

   Residual caveat: `TimedOut` is only published once the backend honours the stop. A backend that
   ignores the stop token never times out at the deadline. Document this in §4. See
   `io_service.cpp:600-613`.

1. **`openCapability` appends to `current_->opened` on every provider call**
   (`script_runner.cpp:559-562`). A second evaluation of the same relation yields a duplicate and so
   a second `closeEndpoint`, which is refused `Conflict`. Dedupe on insert. When `current_` is null
   the capability is handed out and never closed.

1. **Finished rows are retired after the first admission.** `openFile` and `bridge.submit` run
   before `retireAll(finished_)` (`script_runner.cpp:518` and `:689`). A prior script that filled
   the budget makes the next script's first action `BudgetExceeded` although its rows were
   retirable. Retire before admitting.

1. **Two clocks.** The service clock (`IoService::setClock`) and the runner clock
   (`ScriptRunner::setClock`) are independent, and `setClock` on a service with admitted deadlines
   mixes epochs. Add a note, or have the runner read `host.now()`. `expireDue` also scans every
   request each pump (O(`maxRequests`)): bounded, but worth a line in the plan's cost table.

VERDICT: NOT CONVERGED
