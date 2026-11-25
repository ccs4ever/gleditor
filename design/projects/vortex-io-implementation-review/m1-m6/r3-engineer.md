# Round 3 critique: Software Engineer (Vortex I/O M1–M6 code)

Target: e16ec377 (diff dbe8e40c..e16ec377). Tally: 0 BLOCKING, 2 MAJOR, 3 MINOR. Read-only; nothing
built or run, no TSan, races reasoned from the source.

## Round 2 status

- **M1 (journal sequence after unsynced rename): RESOLVED.** `replaceRecords` returns
  `Replacement{directoryDurable}`; `CaptureJournal::compact` adopts the new image's
  `nextSequence_`/`identityWritten_` before reporting the sync failure
  (`capture_journal.cpp:556-565`, `framed_log.cpp:433-440`), and `compactTo` does the same.
- **M2 (MeshLensChannel `|` framing): RESOLVED.** Bencode lists, exact field count and kinds, a
  refused frame is counted, timed-out pending entries erased (`mesh_lens_channel.cpp:13-62`).
- m1 (durable retain comment), m2 (checkpoint over bound), m3 (noexcept re-link), m4 (reused key),
  m5 (pending map): addressed as the disposition records; nothing further found.

Eviction (r2 architect R2-1): `compactJournal` keeps a durable capture's records only while a
committed slot overlaps, plus everything in `pending_`/`pinned_`/`unpublished_`
(`capture_coordinator.cpp:792-815`). Published receipts and recovery of unresolved attempts do not
read the map, so eviction by itself drops nothing recovery needs. The defect below is in the
compaction, not the eviction rule.

## BLOCKING

None.

## MAJOR

### E1. Owner journal compaction loses a record a worker appended between `recover()` and it

**Problem.** `compactJournal()` takes a snapshot with `journal_.recover()` (mutex taken and
released), computes `keep` from it, then calls `journal_.compact(keep)`, which takes the mutex again
and rewrites the whole file from `keep` (`capture_coordinator.cpp:782`, `:837`;
`capture_journal.cpp:535-566`). Since P3 the `Appended` and `Committed` records are written by
worker threads (`capture_coordinator.cpp:1612`, `:1738`). The two journal calls are not one critical
section, and compaction does not re-read the file.

**Failure scenario.**

1. Capture X is in step 1 (`CommitPending`, worker about to append `Appended(X)`).
1. The owner calls `prepare()` for capture Y; the journal bound refuses the hold, so
   `compactJournal()` runs (`:1203-1219`). `recover()` returns the records without `Appended(X)`.
1. The worker acquires the mutex, fsyncs `Appended(X)`, releases it, and reports success.
1. The owner's `compact(keep)` acquires the mutex and renames a file without `Appended(X)` over the
   journal, resetting `nextSequence_`.
1. Step 2 appends `Committed(X)` for a capture with no `Appended`. A crash after step 1 but before
   step 2 leaves X's bytes durable in the permascroll with no record of them; after step 2, recovery
   meets a `Committed` with no `Appended`.

The same window exists for a worker `Committed` of an earlier capture. The data survives only in the
permascroll; the journal's crash-recovery evidence for it is silently gone.

**Evidence.** `capture_coordinator.cpp:782-838`, `capture_journal.cpp:535-566`,
`capture_journal.hpp:217-224` (the header claims writers "serialize on one mutex", which holds per
call, not across recover-then-compact).

**Fix.** Make compaction one critical section: pass a keep-predicate to `CaptureJournal::compact`
that is evaluated on the image read under the same lock, or have `compact` re-read the file and
refuse (`Stale`) when the record count differs from the view the caller computed from. Alternatively
refuse compaction while any `pending.durable` is set. Add a test that appends from a second thread
between the two calls (the `crash` hooks give a deterministic seam).

### E2. A Buffered `Store::save()` advances `durableOps`; a later Fsync snapshot skips the fsync

**Problem.** `SnapshotWriter::write` advances the shared `durableOps` after a Buffered write (page
cache only; `store.cpp` `channel.durableOps.store(snapshot.endOps())`). A later `Fsync` snapshot of
the same Store and directory whose operations are all `<= durable` skips the whole ops block
(`if (snapshot.endOps() > durable)`), and skips the tables image when a later sequence already wrote
one. It then fsyncs only the directory. Nothing fsyncs `ops.nodes` or `store.tables`, yet
`CapturePublisher::write` returns success and step 2 appends `Committed`.

**Failure scenario.**

1. `continueCommit` takes the step-2 snapshot S (seq 5, ops through N) and submits the worker step.
1. Before the worker runs, the owner (or any holder of the same `Store`) calls `Store::save(dir)`,
   as the editor does: snapshot seq 6, Buffered write, `durableOps = N`, `tablesSequence = 6`,
   tables written with plain `writeStoreTables`, no fsync.
1. The worker writes S: `durable (N) >= S.endOps()` so ops are skipped; `S.sequence (5) <= 6` so
   tables are skipped; `syncDirectory`; returns true.
1. `Committed` is fsynced. Power loss: the journal says committed, `ops.nodes` and `store.tables`
   were never flushed; the publication is gone and recovery trusts the `Committed` record.

**Evidence.** `store.cpp` `SnapshotWriter::write` (skip block, `tablesSequence` check, and
`durableOps.store`), `store.cpp` `Store::save` (Buffered), `capture_publication.cpp` `write()`
(default Fsync). No test mixes a Buffered save with an Fsync snapshot (`store_snapshot_test.cpp`).

**Fix.** Track `syncedOps`/`syncedTablesSequence` apart from `durableOps`, and when the requested
durability is Fsync and the directory's bytes are only buffered, fsync `ops.nodes` and rewrite or
fsync `store.tables` before returning true. Or have `save()` use the same durable-count semantics
and never let a Buffered write advance the count an Fsync writer trusts.

## MINOR

### m1. Replace over an existing `ops.nodes` truncates in place: a crash destroys Committed captures

`writeSegmentRange(Replace)` opens with `O_TRUNC` and rewrites (`segmented_ops_spool.cpp:777-790`).
Replace is reached routinely after a `BaseMismatch` reset (the documented "one retry"). A crash
after the truncate leaves a short file while earlier `Committed` records claim those operations.
Write whole files to a temp and rename when `sync` is requested.

### m2. The per-directory mutex is per Store and per channel, not per directory

`snapshotChannel_` is replaced when `saveSnapshot` names another directory, or on `load()`
(`store.cpp` `saveSnapshot`, `load`), and two `Store` objects (or two spellings of one directory:
`absolute().lexically_normal()` does not resolve symlinks) get separate channels and mutexes. A
writer still in flight on the old channel and a Replace through the new one then write `ops.nodes`
concurrently (`O_TRUNC` against `Extend`). The P3 text says "per-directory mutex". Key a
process-wide registry by canonical path.

### m3. Retiring a capture mid-step releases its journal hold while the worker still appends

`retire()` calls `journal_.unhold(pending.heldJournalBytes)` (`capture_coordinator.cpp:683-690`)
though step 1 or 2 may still append `Appended`/`Committed` under that hold. Until the worker
finishes, the owner can hold the same bytes for another capture, so a later `appendRecord` can fail
`BudgetExceeded` after its bytes were already promised durable (Appended failing after the
permascroll sync). Defer the unhold to the step's end, as `completeAfterTurn_` does for the key.

## Reviewed, no defect shown

Atomics: `durableOps`/`maxCaptureId_`/`writtenBytes_` are default seq_cst or relaxed size hints; the
owner reads `durableOps` only to choose a copy base, and writers store it after the write and under
the mutex, so the order is sound. In Extend, nodes are fsynced before the count
(`segmented_ops_spool.cpp:810-815`), so a reader cannot see a count with no nodes in Fsync mode; a
failed extension leaves extra nodes that make the next Extend a `BaseMismatch`, which falls back to
Replace. Out-of-order writers: the `sequence_ > tablesSequence` and skip-overlap logic keep either
order consistent. The `save()`/`CapturePublisher::save()` retry-once is bounded and correct. Lost
wakeups: the coordinator never waits; `continueCommit` is poll-driven. Abandoned commit: `commit()`
refuses with Conflict 7 until swept; `endTurn`/`completeAfterTurn_` cover the adapter tables.
`stageDurableAt` mirrors `stageAt`'s key checks.

VERDICT: NOT CONVERGED
