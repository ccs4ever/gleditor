# Round 2 critique: Vortex Expert (M7-VQL-b, bda0e78b..1a2f94c0)

Tally: BLOCKING 0, MAJOR 0, MINOR 4. Verdict: APPROVE. Read-only review, tests not executed.

## r1 findings

- **M1 (expire orphaning).** I cannot show a script that reaches the orphaning path. It needs a
  scoped choice point (`vortex_vm.cpp:1568`, `abortScopedChoices`) held across the await, and the
  fixer's claim that no statement shape leaves one is consistent with `execution.cpp:595-615`, where
  only the failure path aborts scopes. I did not find a counter-example, so I do not insist on a
  fail-before test. The reorder is still correct and costs nothing. `cancelRequest(token.request)`
  runs before `cancelWaiter` and before `parked.reset()` (`script_runner.cpp:960-966`), so a later
  `rolledBack` orphan finds a request already being stopped. The diagnostic text is now true.
  Closed.
- **M2 (close at budget).** The exemption is sound (`io_service.cpp:224`).
  - Growth is bounded by endpoints, not unbounded. A close needs an open endpoint, and
    `closingEndpoints_` refuses a second close at once with `Conflict`. A script's opens each hold a
    budget slot until it ends, so closes issued at its end number at most `maxRequests`. Across
    scripts the extra requests are bounded by live endpoints, i.e. descriptors.
  - The refused-close retry terminates. A refusal is `Conflict` or `SchedulerUnavailable`.
    `Conflict` clears once the other close completes, because `closeCapability` then errors and
    `closeOne` returns true (`script_runner.cpp:266-271`). `~ScriptRunner` forces and drops
    (`reapClosing(true)`). Closed.
- **M3 (destructor).** `retireAll(..., true)` orphans running requests (`script_runner.cpp:150-167`,
  `retire` at `:218-235`). `orphanRequest` counts them in `orphanCount_` (`io_service.cpp:679`) and
  `forgetOrphan` frees them at their end. Closed.
- **Deadline index.** Every path that erases from `requests_` unindexes first: `eraseRequestState`
  (`io_service.cpp:407`) and `forgetOrphan` (`:695`) are the only two erase sites. `expireDue`
  removes the entry before examining it, and the later `unindexDeadline` then finds nothing, which
  is harmless. Token values come from `token.value`, a service counter, so a slot or ID reused by a
  later request cannot match a stale entry. An orphaned running request keeps its entry, so it is
  still stopped at its deadline. I found no inconsistency.
- **Clock and deadline rule.** The runner reads `IoHost::now()`, which is `IoService::now()`, so the
  two cannot disagree. Sums stay in range: each term is at most max/4 (`script_runner.hpp`
  `maxDeadline`). The rule "request deadline + timeout" is applied in `admit`
  (`script_runner.cpp:775-782`).

## MINOR

1. A forced orphan on destruction is not cancelled, so a read on a backend that never ends keeps its
   `maxRequests` slot for the life of the service. This is documented as a deferral. When the runner
   is gone nobody can read the outcome, so a `cancelRequest` before the orphan would be cheap.
   Consider it for M8.
1. The comment at `io_service.cpp:728-730` ("as large as the closes the admission budget allows at
   once") is stale now that closes are exempt from the count. Reword it to "bounded by open
   endpoints".
1. The script deadline extension (`script_runner.cpp:775-782`) is permanent for the script. One long
   `within` lets every later, unbounded await in that script wait up to the larger deadline.
   Document it, or extend only while that request is the awaited one.
1. A close that stays refused (`SchedulerUnavailable`, or a bridge `match` failure) is retried at
   every `reapClosing`, minting and retiring a refusal cell each time (`script_runner.cpp:296-303`).
   It is bounded and ends on force, but a retry count or a "service shutting down" stop would remove
   the churn. The retry has no test (the disposition says no script shape produces a refusal).
