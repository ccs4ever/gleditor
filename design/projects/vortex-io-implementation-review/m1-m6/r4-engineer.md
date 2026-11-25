# Round 4 critique: Software Engineer (Vortex I/O M1-M6 code)

Target: 720fef61 (diff e16ec377..720fef61). Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR. Read-only; nothing
built or run (TSan result taken from the disposition).

## Round 3 status

- **E1 RESOLVED.** `compactWhere` scans, selects and rewrites under one hold of `mutex_`
  (`capture_journal.cpp` compactWhere/recoverLocked/compactLocked). The selector touches only owner
  state (`pending_`, `pinned_`, `unpublished_`, `translations_` via `hasCommittedSlotOver`); it
  never calls the journal, so no re-lock deadlock. Workers only wait on `mutex_` for the owner's
  rewrite; the owner never waits on a worker except for that mutex, so no cycle. The test appends
  from a thread started inside the selector; that append must block on `mutex_` until after the
  rename, so it does exercise the interleaving (record lands after compaction, asserted).
- **E2 RESOLVED.** `syncedOps` and `syncedTablesSequence` are separate from the written counts. A
  Fsync write that skips its ops block still fsyncs `ops.nodes` when `syncedOps < endOps`, and the
  tables when `syncedTablesSequence < sequence_`, before the directory fsync. Buffered Replace and
  load reset to 0; the Buffered tables write zeroes the synced sequence. The `SnapshotSyncs` seam
  lets the test observe the fsyncs rather than infer them.
- **m1 RESOLVED.** Replace writes `ops.nodes.tmp`, fsyncs when asked, renames, removes the temp on
  every failure; the closing `syncDirectory` makes the rename durable. `store.tables` is now atomic
  for Buffered too. Stale-temp cleanup at open does not exist (see minor 1) but a stale temp is
  overwritten by the next Replace and the loader does not read it.
- **m2 RESOLVED.** Registry is guarded by one static mutex; entries are weak and pruned on each
  call. Inode reuse: a recreated directory with the same inode while the old channel is alive shares
  stale counts, but the writer re-checks the file (missing or short gives
  `durableOps=0, syncedOps=0`, returns false, caller retries as Replace), and `nextSequence` is
  shared so tables are never skipped as stale. No wrong result found.
- **m3 RESOLVED.** No window between the turn taking its staged step and `retire()`: `execute` moves
  the step out of `durableStaged_` and sets `running_` under the adapter mutex, and `complete()`
  erases staged entries under the same mutex, so either the turn is running (hold deferred) or it
  will never run. `releaseDeferredHolds` runs from `sweepDead`, which `prepare` calls first, so held
  bytes are freed before the next bound check.

## New defects in the r3 diff

None BLOCKING or MAJOR.

### Minor 1. No stale `ops.nodes.tmp` cleanup at open

A crash between the temp write and rename leaves `ops.nodes.tmp` (a store directory is documented as
exactly three files). Harmless to correctness; cleared by the next Replace. Remove it in `load()` or
document it.

### Minor 2. `channelFor` does filesystem metadata calls on the owner

`create_directories`, `weakly_canonical` and `stat` run in `saveSnapshot` (owner), and `load()`
takes the channel mutex, which can wait on a worker's fsync. Disclosed in the code comment and
bounded to once per snapshot; priced, not proven a stall.

VERDICT: CONVERGED (no BLOCKING or MAJOR)
