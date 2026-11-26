# M7-VQL gate round 3: Vortex expert (commit 183129f5)

Tally: BLOCKING 0, MAJOR 0, MINOR 3. Verdict: APPROVE.

## MINOR

1. `ScriptRunner::orphan` (`script_runner.cpp:167-186`) drops a parked script's waiter and erases
   its `parked_` entry but queues no wake and no diagnostic. The script's activation stays in
   `live_` with `parked` set, and a caller driving scripts by `takeWake()` never hears of it. The
   state is correct: a script whose Submit sits above an undone choice point has lost its cells, so
   the request cannot be resumed, and a same-request resume is not violated for a script whose own
   choice did not roll back, because `rolledBack` matches by submission. A resume of the stale token
   fails at the tokenStale check (`script_runner.cpp:~425`). But `ARollbackNamesTheActivation...`
   (`vql_io_script_test.cpp:668`) asserts only `orphanCount == 2`. Push a `ScriptWake` with an
   abandoned reason, or say in the header that the token goes stale silently.
1. `clearCursorState` (`execution.cpp:~556`) tests `arena.contains(cursor)`, which is by ref only.
   The cursor is freed only by `close()`/reclaim, and the throw path precedes both, so reuse by
   another live cursor cannot happen inside `advance`. `matches(cursor, incarnation)` would still be
   the right test, and costs nothing. The throw path has no test; it is a noexcept cleanup with a
   logged fallback, so the missing test is low risk.
1. `deleteCell`/`detachCell` default to no `CloneGuard`. The visualizer
   (`zigzag_visualizer.cpp:615`) guards. The io_cells reapers use `heldByViews` and are correct for
   their case: they keep the held cell and its subgraph, which no plan-level guard can express. The
   program reclaim (`execution.cpp:315`) passes no guard. It is safe today because views reachable
   from kept rows keep the master, and views among owned cells are freed together. A third caller
   forgetting the guard is the footgun. Consider requiring the argument (explicit `std::nullopt`)
   rather than defaulting it.

## Checked and clean

- `orphanRequest` result and `orphanCount` are correct. An orphaned open mints no endpoint, and
  `openSettled(token, nullopt)` makes `FileEndpointAdapter` erase the pending open and drop the
  descriptor (`file_endpoint_adapter.cpp:298-325`). There is no leak.
- `rolledBack` searches the current activation, then `live_`, and removes the entry by submission.
- `keepIfNamed` and the registry counts are sound.
