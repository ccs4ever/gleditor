# Round 2 critique: Vortex Expert (M7-VQL gate, focused)

Tally: 0 BLOCKING, 0 MAJOR, 5 MINOR. Target be30c946 (diff 8711aa1c..be30c946). Verdict: APPROVE.

## r1 status

- M1 (backtracking over Submit): resolved. `releaseChoiceMark` (`vortex_vm.cpp:1550`) notifies
  newest first before `arena.release`, and is used by backtrack (:1466), scoped abort (:1583),
  `abortOwnedMutationGroups` (:494) and the failed mutation-group path (:2383). `cut` and `discard`
  keep the cells, so they correctly announce nothing. Entries left after a cut are harmless: an
  older choice's `submissionMark` is below them and a Submit under an empty stack clears the list.
  The runner orphans (`script_runner.cpp:rolledBack`), the service runs the request to its end,
  publishes nothing and forgets it (`io_service.cpp:orphanRequest/forgetOrphan`, pump skip). No
  retry, and re-execution mints a new operation. Accounting is bounded by the service budget.
  `ARetryLoopOverASubmitLeavesTheServiceAndArenaBounded` covers it. VM destruction announces
  nothing, but the runner destructor retires or orphans every live activation's entries.
- M2: resolved (`Activation::admitted`, `Stepping`, `current_`), five-script interleave test.
- M3: resolved (see below). No race found with a live continuation, parked cursor or the rollback
  handler: reclaim runs only at `ProgramRun::close()`, after the parked mark was discarded, windows
  are monotone incarnation ranges (incarnations never rewind on release), host-allocated cells are
  excluded, and rows plus what they reach by links are kept. Freeing under an outer mark is
  journalled and restored by release.
- r1 MINORs 1 and 2 fixed. Minors 3 fixed. 4 to 8 skipped; accepted.

## P7 review

Plan (`cell_deletion.cpp:planRankDeletion`) is correct for linear and circular ranks: Repair on a
rank of three or more writes `neg.pos := pos`, which displaces the cell from both sides; a one-cell
loop is cleared once; no idempotent write is emitted. `deleteCell` refuses before writing
(`arena_manifold.cpp:3042`), `detachCell` reads the run before writing, free follows detach so
`CellLinked` cannot occur. Under a mark both the link writes and the free are journalled. Shadowed
base cells are detached, not freed. Persistent detach in `deleteFocusCell` is one `SetLink` per
dimension, minting nothing, which respects R7/R8. The io_cells reapers and xuzz share the plan.
Repair keeps every non-degenerate rank at degree two or one-ended; no new self-links.

## MINOR

1. `rolledBack` (`script_runner.cpp`) looks only in `current_->admitted`, and returns silently when
   `current_` is null or the entry belongs to another activation. If another script's Submit sits
   above the rolled-back choice (interleaved parked scripts), that request is never orphaned and
   stays in the budget. Search `live_` too, and log a miss.
1. Orphan visibility: `IoService::orphanRequest` returns silently when the request has waiters, and
   there is no orphan count or debug log, so a stuck orphan is invisible. Add a `GLEDITOR_LOG_DEBUG`
   and an `orphanedCount()` accessor.
1. `io_service.cpp:776` (`mintOpenedEndpoint`) still runs for an orphaned request in pump and mints
   cells nothing can name or reap. Skip it when `state.orphaned`.
1. The exception catch blocks in `QueryExecutor::advance` (`execution.cpp:~646-659`) do not call
   `setCursorOpcode(cursor, nullopt)` as the failure branch does. With P7 the cursor slot is now
   freed and its `CellRef` reusable, so stale `activationFrames_`/`guardStacks_` entries for it
   could attach to a later program. Run the same per-cursor cleanup there.
1. Spec notes, not defects. The two-cell circular rank leaves the survivor unlinked (old xuzz
   self-linked it); both are a rank of one, so this is consistent with ZigZag and is documented. The
   middle fallback (other connections, posward first) reads the owner's "first bound vector"
   consistently and only deviates for unbound-only connections; the posward-before-negward order is
   an assumption inherited from xuzz and should be named in the keymap docs. Deleting a clone master
   promotes its first view without checking its content; callers (`heldByViews`) must guard.
