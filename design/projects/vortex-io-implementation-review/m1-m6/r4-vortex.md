# Round 4 critique: Vortex Expert (focused)

Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR. Target `720fef61` (diff `e16ec377..720fef61`).

## r3 B1 (compaction erasing a worker's record): RESOLVED

- Single critical section: `compactWhere` takes `mutex_` once, scans via `recoverLocked`, runs
  `select`, then `compactLocked` (`capture_journal.cpp:539-556`). Worker appends take the same
  mutex, so they land wholly before the scan or after the rename, never between.
- Callback state: it reads `pending_`, `pinned_`, `unpublished_` and `translations_`
  (`capture_coordinator.cpp:828-866`, `hasCommittedSlotOver` `:796`), all owner-only.
  `compactJournal` is called only from `prepare` (`:1254`, `:1267`), on the owner. Workers touch
  only the sink, the journal and the adapter's own mutex-guarded tables, never those maps. No race,
  and compaction stays owner-only.
- Deadlock: the owner holds the journal mutex only inside `compactWhere` and never waits on a worker
  or on the adapter mutex there (the callback calls no journal or service method). A worker blocked
  on the journal mutex needs nothing from the owner, so it proceeds once the owner finishes. The
  owner waiting on a worker's fsync is bounded, and the header documents it. No cycle.
- A retired capture whose step is still running is not "live" and its intent may be dropped. A later
  `Appended` from that step is then an orphan, which `recover()` already counts as orphan bytes. It
  is not new, and it concerns a dead capture.

## r3 MINOR (retireRequest on in-flight step requests): RESOLVED

- `retire()` calls `service_.retireRequest` (`:709-714`). An unfinished request takes the documented
  road (`retireAfterTerminal` plus `cancelRequest`, `io_service.cpp:587-591`), and the state is
  erased at its terminal. This is the contract, not a violation.
- The hold is unaffected: `adapter_.complete(key)` marks `completeAfterTurn_` if the turn is
  running, and the deferred hold is keyed on `turnRunning`. Both `execute` (`running_.insert`) and
  `complete` run under the adapter mutex, so there are only two orderings. A turn that has not
  started finds its staged entry erased and appends nothing, so the hold is freed immediately and
  safely. A turn already running is deferred, and `sweepDead` frees the hold after `endTurn`
  (`:652-657`). No stranded hold.

## New defects in the r3 diff (my lane)

None. `function_ref` is called synchronously with a lambda that outlives it. Detail 6 only sets the
failure detail. Request and outcome handling in `continueCommit` is unchanged.

VERDICT: CONVERGED
