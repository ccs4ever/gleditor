# P3 implementation: capture commit durability off the owner

Owner-approved core proposal P3 ([`r1-disposition-b.md`](r1-disposition-b.md) "Proposal register",
[`r1-architect.md`](r1-architect.md) A2), implemented on `vortex-io-m5-m6`. Plan §14 forbids "any
path that blocks the evaluation/UI thread"; the capture commit ran the permascroll sync, two journal
fsyncs and a whole `Store::save` on the owner. It now runs none of them.

## Design

### The store half (core: `store.hpp`, `store.cpp`, and `segmented_ops_spool` where needed)

- `Store::saveSnapshot(dir)` runs on the owner and copies what `dir` does not durably hold into an
  immutable `StoreSnapshot`: the operation nodes above the directory's durable count, the node the
  file must end with, the `store.tables` image, a sequence number and a handle to the permascroll.
  It costs O(operations added since the directory last became durable), not O(store).
- `SnapshotWriter::write(snapshot, durability)` runs on any thread. It flushes the permascroll,
  writes `ops.nodes` and `store.tables`, and removes the files `store.tables` superseded. Nothing in
  it reads the `Store`.
- `Store::save()` is `saveSnapshot()` then `SnapshotWriter::write(Buffered)` on the calling thread,
  retried once as a whole-store snapshot when the extension was refused. There is one path to both
  files. `Buffered` is what `save()` always meant (page cache); `Fsync`, which the capture uses,
  also fsyncs both files and the directory and replaces `store.tables` by write-fsync-rename.
- **One writer of `ops.nodes`.** `SegmentedOpsSpool::writeSegmentRange()` (static, over a span of
  nodes) is the only code that writes a segment file's bytes; `writeSegmentFile()` is that function
  over the whole spool with `Replace`. The snapshot writer calls it with `Extend` for the increment,
  nodes first and the count last (the order `flush()` already uses, so a crash leaves a header that
  undercounts whole nodes, which `readSegmentShape()` recovers). Touching `segmented_ops_spool` was
  strictly needed: the header and shape helpers live in its anonymous namespace.
- No format change: `ops.nodes` and `store.tables` are byte-identical to what `save()` wrote (a test
  compares them), so no fixture under `tests/samples` is affected and `sizeof(CompactOpNode)` is
  untouched.

### Questions the task named, and the answers in code

- **Concurrent snapshots: serialize or chain?** Chained at snapshot time, serialized at write time.
  A snapshot copies from the count the directory _durably_ holds, never from the count last
  snapshotted, so a snapshot taken while an earlier write is in flight carries the earlier one's
  operations and stands alone. Writes into one directory take a per-directory mutex (held across the
  fsyncs, never taken by the owner); a writer skips the operations a concurrent writer already made
  durable and skips a `store.tables` image older than the one on disk, so either write order leaves
  the directory consistent. The durable count is an atomic a writer advances only after its write
  finished: a refused or crashed write leaves it where the directory really is, and the retry copies
  from there.
- **Store mutated after the snapshot.** The snapshot copies the nodes it carries into its own vector
  and holds the tables by value, so nothing aliases the arena or the Store's containers. Sharing
  sealed segments by reference was considered and refused: a segment can be unmapped, the tail is
  still being appended to, and the increment is what is written anyway, so a refcount would buy a
  lifetime problem to save a copy of a delta.
- **Wrong picture of the directory.** The extension checks the file's operation count and that its
  last node equals the snapshot's recorded tail, byte for byte; the writer also stats the file when
  it believes it holds operations. On any mismatch it changes nothing, resets the directory's count
  to zero and returns `false`; the next snapshot is the whole store. `load()` sets the count to what
  it read; `saveSnapshot()` to another directory starts at zero.
- **`Store::save()` callers.** Unchanged signature and result; a test writes the same store both
  ways and compares the files.

### The capture half (`capture_coordinator`, `capture_publication`)

`commit()` is split. The durable order on disk is the documented one: `Appended` (after the sink
sync) -> publish -> save -> `Committed`.

| step                              | thread | work                                                                                                                                     |
| --------------------------------- | ------ | ---------------------------------------------------------------------------------------------------------------------------------------- |
| `commit()`                        | owner  | validate the append outcome as before; refuse if a durable step is in flight; submit step 1; return `CommitPending`                      |
| step 1 (`CaptureDurable` request) | worker | permascroll `sync()`, then the fsynced `Appended` record                                                                                 |
| `continueCommit()`                | owner  | check the step's outcome, read its report; publish into the Store (memory only), take the snapshot, mint the receipt cell; submit step 2 |
| step 2 (`CaptureDurable` request) | worker | write and fsync the snapshot (`CapturePublisher::write`), then the fsynced `Committed` record                                            |
| `continueCommit()`                | owner  | check the outcome; commit the translations, retire the capture; return `Committed`                                                       |

- The durable steps ride the existing machinery: the same `CaptureAppendAdapter` and adapter key
  (`stageDurableAt`), so a host registers nothing new, and the same retired-turn rules for the
  deferred cleanup. A step's result is a `CaptureDurableReport` (typed failure, or an exception
  pointer, plus the journal sequence). A failure refuses the continuation and the commit is
  retriable: publication is idempotent by `(journalId, CaptureId)`, a retry skips step 1 when
  `Appended` is durable, and a retry takes a fresh snapshot. An escaped exception (a crash point in
  tests) is rethrown by `continueCommit()`, as the same hook was rethrown by the synchronous commit.
  A step cancelled before it began has no report and refuses with `SchedulerUnavailable`.
- The step's request is the coordinator's own, so `continueCommit()` retires it
  (`service_.retireRequest`): a commit is now three requests, and an unretired one would spend a
  host's request budget three times as fast. (The existing 60-capture bound tests, run against a
  64-request service, found this.)
- `CaptureJournal` is now used from workers while the owner holds budget for other captures. Appends
  (`appendRecord`), `compact` and the recovery scan serialize on one mutex held across the fsync;
  the budget calls (`hold`, `unhold`, sizes) take no lock and read atomics the writers publish after
  each durable record (`FramedLog`'s written size and header flag, the identity flag, the max
  `CaptureId`), so the owner never waits for a worker's fsync. `heldBytes_` stays the owner's alone.
- `CapturePublisher` gains `snapshot()` (owner), `write()` (any thread) and a `setSnapshotWriter()`
  test seam; `save()` is snapshot-then-write with the one retry, used by recovery.

### What still blocks the owner, and why

`appendRequest()` does not move its `Append` intent fsync, and this is deliberate rather than
unfinished. The intent is the only durable evidence that bytes may exist, and recovery's attempt
state machine, the certain-abort path (an `Aborted` record closing an intent that an admission
refusal proved never ran) and every restart test read it as "durable before submit". Running it in
the append's worker turn would turn an admission refusal into an `Aborted` record with no intent
(ambiguous to recovery) and would still leave the `Aborted` fsyncs of the failure paths on the
owner. Also on the owner: journal compaction under bound pressure, and `recover()`, `reconcile()`
and `resumePublications()`, which run at open or on an explicit owner decision with nothing else in
flight (they save through the same snapshot writer on the calling thread). None is on a commit's
happy path. Moving the intent would be a journal-protocol change and wants its own proposal.

## Files

- Core: `apps/common/xanadu/store.hpp`, `store.cpp`; `segmented_ops_spool.hpp`, `.cpp` (the range
  writer and `copyOpsAfter`; stated as strictly needed above).
- Capture: `apps/common/xanadu/vortex/capture_coordinator.hpp`, `.cpp`; `capture_publication.hpp`,
  `.cpp`; `capture_journal.hpp`, `.cpp`; `framed_log.hpp` (two atomics).
- Tests: `tests/xuzz/store_snapshot_test.cpp` (new), `tests/xuzz/io_capture_test.cpp`.
- Docs: `design/vortex-io-implementation-plan.md` (M6 notes; the §14 deviation is resolved for the
  commit and the remainder named).

## Tests

`store_snapshot_test.cpp` (`StoreSnapshotTest`, `SegmentedOpsSpoolRangeTest`):

- `aSnapshotWriteEqualsASynchronousSaveByteForByte`: `ops.nodes` and `store.tables` equal a
  synchronous `save()`'s, and a reload has the same operations.
- `anIncrementalSnapshotCopiesOnlyWhatTheDirectoryLacks`: base and node count; the extended file
  equals a whole save's.
- `aStoreMutatedAfterTheSnapshotDoesNotLeakIntoIt`, `aSnapshotIsWrittenWhileTheOwnerKeepsEditing`
  (writer on a thread while the owner appends).
- `concurrentSnapshotsStandAloneInEitherWriteOrder`,
  `twoWritersOnTwoThreadsLeaveOneConsistentDirectory`.
- `aRefusedWriteDoesNotAdvanceWhatTheDirectoryHolds`,
  `aDirectoryEmptiedBehindTheStoresBackIsWrittenAgain`,
  `aLoadedStoreWritesOnlyWhatItAddsAfterTheLoad`, `anotherDirectoryGetsTheWholeStore`,
  `anExtensionOfAForeignFileIsRefusedNotAppended`, `extendingAFileThatIsNotThereIsAMismatch`.

`io_capture_test.cpp` (`IoCaptureTest`; the existing tests now drive `commit()` through
`commitToEnd`, which runs each step's worker turn and continues):

- `CommitDoesNoBlockingIoOnTheOwnerThread`: sink sync, every journal fsync after the intent, and the
  snapshot writer all record their thread; none is the owner's, and between `commit()` and the
  worker turns there is no `Appended` record, no `ops.nodes` and no `Committed` record.
- `AStoreEditedWhileItsSnapshotIsInFlightIsNotTornIntoIt`.
- `TwoCommitsInFlightTogetherLeaveBothDurable` (both steps on concurrent threads),
  `TheSecondSnapshotWrittenFirstStillLeavesBothDurable`.
- Crash and refusal between each durable step:
  `CoordinatorJournalCrashPointsBracketTheAppendedFsync` (before and after `Appended`),
  `CrashBetweenAppendedAndPublicationPublishesExactlyOnce`,
  `CrashDuringTheSnapshotWriteRecoversByPublishingAndSaving` (new: the write throws, recovery
  publishes, saves, commits), `CrashBetweenStoreSaveAndCommittedRecoversWithoutRepublish`,
  `ARefusedSnapshotWriteLeavesTheCommitRetriable` (new),
  `CommitRetryAfterRefusedStoreSaveNeverPublishesTwice`,
  `ACommitWhileAStepIsInFlightIsRefusedNotDoubled` (new).

## Counts

Headless, `make`'s environment. Focused (`IoCaptureTest.*`, `StoreSnapshotTest.*`, `*Io*`,
`*Capture*`, `*Store*`): 558 tests, 557 passed, 1 skipped (pre-existing). Full `xuzz_test`: 4785
run, 4756 passed, 28 skipped (network, Postgres and opt-in profiles, as before), 1 failed:
`ArrayfiladeBenchmarkTest.Subscripting10000Cells`, a timing threshold (speedup 4.1 against a
required 5.0) that also failed on the first full run, before any capture change, and passes in
isolation (8.5x to 9.1x, three runs); it is not a P3 regression. `zigzag_test`: 268 of 268 passed.
New tests: 12 in `store_snapshot_test.cpp` (11 `StoreSnapshotTest`, 1 `SegmentedOpsSpoolRangeTest`),
7 new `IoCaptureTest` cases (70 now, 63 before). `gleditor_test` was not built or run: the library
(`src/`, `include/`) is untouched. No fixture under `tests/samples` changed.

## Not done

- `appendRequest()`'s intent fsync, for the reason above.
- No ThreadSanitizer run; the concurrent tests were repeated 100 times under the normal build with
  no failure.
- `store.tables` is still rewritten whole on every snapshot (it is small and it is what makes the
  snapshot self-consistent); only the operations are incremental.
