# P4 and P5 implementation: per-cell arena release, and the capture intent off the owner

Owner-approved core proposals P4 and P5 ([`r2-disposition.md`](r2-disposition.md),
[`r3-disposition.md`](r3-disposition.md) "Proposal register"), implemented on `vortex-io-m5-m6`. P4
changes `ArenaManifold` (a core file); P5 changes the `capture.journal` protocol.

## P4: per-cell release in `ArenaManifold`

### What it does

`ArenaManifold::freeCell(ref, incarnation)` frees one cell. The slot goes on a LIFO free list and
the next mint reuses it under a new incarnation, so an arena that serves requests is the size of its
peak live cells and not of its history. The request reaper (`reapIoRequest`) calls it for the whole
request graph.

### Design decisions

- **Free list, not tombstone plus `compact()`.** A `CellRef` is the slot's index. `compact()` moves
  runs, never slots, so it can reclaim a freed cell's link and content runs and can never reclaim
  its slot; tombstones alone would leave the arena the size of its history. The free list is the
  only shape that bounds the slot count. Runs are still reclaimed by the existing `compact()` (and
  by the automatic compaction `linkFor`/`setContentAt` already run when no mark is outstanding).
- **Cost, measured.** `freeCell` is O(1) plus a scan of the arena's dimension names (a few tens,
  never cells). A mint that reuses a slot is O(1). `anArenaServingAStreamStaysTheSizeOfItsLiveCells`
  drives 20,000 cycles of two mints, a link, an unlink and two frees through one arena in about 7 ms
  with its assertions, roughly 0.35 microseconds a cycle. `denseOf`, the root of every read, gained
  one test of the slot's flag byte; the slot is read by every caller next, so it is the same cache
  line (a parallel vector of incarnations would have been a second one). Nothing on the read path
  became O(N).
- **Freed means unnamed.** A freed slot is an empty slot with `ArenaManifold::freedSlotFlag` set in
  the previously unclaimed `CellSlot::flags` byte. `denseOf()` answers `nullopt` for it, so
  `contains()`, `slot()`, `incarnationOf()`, `linked()` and `link()` treat the old ref as a cell
  never minted. `cells()` still lists the slot (it is a span over the slots), as an empty slot with
  the bit set.
- **Incarnations are the safety net.** An incarnation was already monotonic and never reused. A
  reused slot takes a new one, so `matches()`, `setOwnerLease()`, `ownerLeaseOf()`,
  `isRuntimeOnly()` and `runtimeTagOf()` refuse a stale handle, and `freeCell()` takes the
  incarnation so a stale handle cannot free the cell that reused its slot. Freeing drops the cell's
  owner lease (a payload or observation lease is released with it), runtime-only mark and tag, so
  none leaks into the next tenant.
- **Refusals, each changing nothing.**
  - `UnknownCell`: not a live cell of this arena, or the incarnation is stale. A base cell the arena
    only reads is this.
  - `CellLinked`: the cell has a live link on any dimension. The caller detaches first, which keeps
    every rank two-degree: a freed cell is never the neighbour a rank walk lands on. A link is
    recorded in both cells' runs, so checking the cell's own run is enough.
  - `NotReleasable`: a copy-on-write shadow (dropping a shadow is the undo, by releasing the mark it
    was taken under; the base is never freed), and any cell other state names: a proxy, quote,
    dimension, provenance cell or space store cell.
- **Marks.** `Mark` gained `freeTrailSize` and `newestIncarnation`. Under a mark the free and the
  reuse are journalled (`FreeTrailEntry`); `release(mark)` undoes them in reverse, which restores
  the free list stack exactly, and the slot headers come back through the existing trail (`freeCell`
  and a reusing mint call `trail()` first). `discard(mark)` keeps them and clears the journal with
  the other trails when the last mark goes. Runtime-only and tag removals are journalled with the
  existing trails (`RuntimeOnlyTrailEntry::wasMarked`, now used). `compact()` stays refused while a
  mark is outstanding.
- **A latent assumption this exposed.** `release()` forgot the runtime state of every incarnation
  newer than `incarnations_.back()`, which assumed slot order is incarnation order. A freed slot
  below the mark that a mint reuses after it holds an incarnation newer than the survivors above, so
  the sweep would have erased live state. It now compares against the mark's own `newestIncarnation`
  (a test pins this: `aFreedSlotBelowAMarkIsNotTruncatedWithTheBranch`).
- **Prefixes.** `exportPrefix()` refuses an arena holding a freed slot (`UnsupportedPrefixState`),
  and the prefix validator refuses a slot with the freed bit: hydrating one would revive it as a
  cell. OR-parallel arenas never free.
- **`cellCount()` is the high-water mark, `liveCells()` the count.** Code that predicts the next ref
  as `refOf(cellCount())` (`mesh_evaluator`) is wrong for an arena that has freed; only the I/O
  arenas free, and those do not.

### The reaper

`reapIoRequest` still detaches the request from its endpoint's queue and the endpoint and capability
clone ranks. It then walks the request's own subgraph (the dimensions only a request owns: endpoint,
capability, outcome, payload, source, occurrence, state, progress, range), detaches any view it
filed on a canonical cell's clone rank, unlinks the owned links, and frees every cell leaf first.

- **A capture keeps its request.** A capture's `Request` field is a view of the request on the
  request's clone rank, and the capture's `commit()` finds the append's result through it. A request
  that still has a clone link is detached from the queue and the field ranks but kept whole until
  the view leaves. The capture's `Request` view leaves only when the capture retires, which is after
  the host has typically retired the append request, so those requests are not freed; this is the
  residual (one request graph per capture, bounded by the capture count, not by traffic). Freeing it
  needs the coordinator to reap after it detaches, which needs to know the service has retired the
  request; not done.
- **Contract change.** The old comment said a retired request keeps its outcome link so a late poll
  still resolves. That cannot hold once the request cell is freed. Retiring is the consumer saying
  it has read the outcome; `retireRequest` frees, a late `poll` finds a stale handle (`{}`), a late
  `await` reports a `Stale` refusal, and the outcome's payload and observation leases are dropped
  with its cells. `IoServiceTest.RetiredRequestPollResolvesOutcomeThroughArenaStore` was the one
  test of the old contract; it is replaced by `RetiredRequestIsFreedAndALatePollFindsItStale`.

### The price, corrected

The plan priced three runtime-only cells (about 200 bytes) per reaped request. Measured through the
real `ArenaIoCellStore`, one served request holds **18 live cells** (request, its endpoint and
capability field occurrences and views, the outcome with its progress fields, and their content
cells), about 1.2 KB, so about 1.2 GB per million requests: six times the priced figure. At the cell
layer alone (a bare outcome) it is 6.

### Files

- Core: `apps/common/xanadu/zigzag/arena_manifold.hpp`, `.cpp`.
- Wiring: `apps/common/xanadu/vortex/io_cells.hpp`, `.cpp` (`reapIoRequest`).
- Tests: `tests/xuzz/arena_free_cell_test.cpp` (new), `tests/xuzz/io_cells_test.cpp`,
  `tests/xuzz/io_service_test.cpp`.

### Tests

`ArenaFreeCellTest` (16, `tests/xuzz/arena_free_cell_test.cpp`):

- `aFreedCellAnswersAsIfItWereNeverMinted`
- `aReusedSlotGetsANewIncarnationAndStaleHandlesAreRefused`: stale `matches`, `setOwnerLease`,
  `ownerLeaseOf`, runtime-only, tag, and `freeCell` by the old handle; the new tenant is untouched.
- `aCellWithALiveLinkIsRefusedUntilItIsDetached`
- `aDimensionAProxyAndAShadowAreNotReleasable`
- `aBaseCellIsNeverFreedNeitherItselfNorThroughItsShadow` (an arena over a `Store`'s base)
- Marks: `aFreeInsideAMarkIsUndoneByReleasingTheMark`, `aFreeKeptByDiscardOutlivesTheMark`,
  `aReuseInsideAMarkIsUndoneAndTheSlotIsFreeAgain`, `aCellMintedAndFreedInsideAMarkLeavesNoTrace`,
  `nestedMarksUnwindFreesAndReusesInOrder`, `aFreedSlotBelowAMarkIsNotTruncatedWithTheBranch`
- `compactionReclaimsAFreedCellsRunsAndKeepsTheRest` (and refused under a mark)
- `promotionNeitherCarriesNorAcceptsAFreedCell` (freed root refused; a reused slot promotes as what
  it now is)
- `aPrefixOfAnArenaThatFreedIsRefused`
- `anArenaServingAStreamStaysTheSizeOfItsLiveCells` (20,000 cycles)
- `aRandomWalkOverMintFreeAndMarksMatchesAModel`: eight seeds of 400 steps over mint, free, mark,
  release and discard against a model of which refs are live, with what text and incarnation.

`IoCellsTest`: `reapedRequestsAreFreedSoTheArenaIsBoundedByLiveRequests` (10,000 append, outcome and
reap cycles: live count and slot count equal what they were after one),
`aReapedHandleIsStaleEvenWhenItsSlotsAreReused`, `aRequestWithAViewOnItsCloneRankIsKeptWhole`; the
existing `reapedRequestsLeaveTheRanksEverySubmitWalks` still passes with its 10,000-request stream.
`IoServiceTest`: `TenThousandSubmitAndRetireCyclesLeaveTheArenaBounded` (the real `ArenaIoCellStore`
through `IoService`; arena live and slot counts equal after 10,000 cycles) and
`RetiredRequestIsFreedAndALatePollFindsItStale`.

## P5: the capture `Append` intent off the owner

### What it does

`appendRequest()` no longer journals or fsyncs anything. The intent is written, and fsynced, by the
append's own worker turn (`CaptureAppendAdapter::execute`), after the turn's last chance to be
cancelled and before it hands a byte to the sink. A refused intent ends the turn `Failed` with no
bytes appended and the key not marked executed, so the owner's `Aborted` is certain.

### Protocol

`capture.journal` is **version 4**; a version 3 journal is refused by number (as 1 and 2 were).

| record                  | writer                                                                                                    |
| ----------------------- | --------------------------------------------------------------------------------------------------------- |
| `Append`                | the append's worker turn, fsynced, before the sink append (was: the owner, before submission)             |
| `Appended`, `Committed` | worker durable steps, as in P3                                                                            |
| `Aborted`               | the owner, on the failure paths (an fsync that stays on the owner, rare and not on a commit's happy path) |

- **Admission refusal writes nothing.** No worker turn ran, so no intent exists and there is nothing
  to close. (The version 3 protocol wrote an `Append` and an `Aborted`.) A refusal spends none of
  the capture's journal hold, so a retry's `topUpHold` is a no-op.
- **Recovery's rule for `Aborted` with no open intent: nothing landed.** Bytes only ever follow a
  durable intent, so with no intent open there are none, whatever the record's uncertain flag says.
  It is no longer an ambiguity; it closes no attempt and pins nothing. After `Appended` or
  `Committed` it remains an anomaly (ambiguous, pinned). A second `Aborted` closes nothing and
  leaves what the first said.
- **The owner/worker race.** When the owner gives an admitted request up (the clone-view link
  failed) it writes an uncertain `Aborted` while the worker's turn is still to come. The journal may
  then read `Aborted, Append`: a new attempt that is open, which recovery pins (`OpenIntent`), the
  same safe direction as `Append, Aborted(uncertain)`, which pins as `UncertainAbort`. In neither
  order does it read as nothing landed. The in-memory pin the owner takes at that moment is
  unchanged.
- **The hold.** The worker writes the intent under budget only the owner may account, so the
  intent's bytes stay held until `commit()` sees the append `Complete` (`intentHoldConsumed`, once).
  Retirement returns whatever is held. A worker whose hold was returned by a dying capture can find
  the file full and refuse its intent: that is a `Failed` turn with no bytes, harmless.
- **Crash ordering.** Intent (worker, fsynced) then sink append, then `Appended` after the sink
  sync, then the snapshot, then `Committed`, as before. A crash before the intent's fsync leaves
  nothing (the record is rolled back); after it, an open intent with no bytes, which recovery pins
  and reports as possible orphans, the existing conservative answer.

### Files

- `apps/common/xanadu/vortex/capture_journal.hpp` (version 4, record documentation),
  `capture_coordinator.hpp`, `capture_coordinator.cpp` (`CaptureAppendSpec::writeIntent`,
  `CaptureAppendAdapter::execute`, `appendRequest`, `commit`, recovery's attempt state machine).
- Tests: `tests/xuzz/io_capture_test.cpp`. Plan: `design/vortex-io-implementation-plan.md` M6 notes.

### Tests

New in `IoCaptureTest` (73 before, 80 now):

- `AppendRequestDoesNoJournalFsyncOnTheOwnerThread`: after `appendRequest()` there is no fsync, no
  record, no byte and the hold is untouched; the worker turn (a thread of its own) does the one
  fsync, on a thread that is not the owner's, and the sink's first append saw the intent already
  durable. Through the thread-recording crash-point seam P3 added.
- `AnAdmissionRefusalWritesNoRecordAndTheCaptureRetries`.
- `ARefusedIntentWriteAppendsNothingAndAbortsCertainly`: death before the intent's fsync; `Failed`
  outcome, no bytes, journal holds one `Aborted`, the range recaptures, and a restart pins nothing.
- `RestartBeforeTheWorkerTurnLeavesNoIntentToReconcile`.
- `AnAbortedWithNoIntentIsACertainAbortNotAnAmbiguity`,
  `AnIntentThatLandsAfterTheOwnersAbortedIsAnOpenAttempt`, `AVersionThreeJournalIsRefusedByNumber`.

Rewritten: `RestartAfterAppendIntentPinsAndReportsOrphanBytes` (the crash seam after the intent's
fsync, before the append), `CertainAbortReleasesSlotsAndLeasesSoTheRangeCanRetry` (a lone
`Aborted`), `ARetiredCapturesJournalHoldOutlivesItsRunningStep` (the hold is read after `commit()`
gives the intent's bytes back), and a comment in `CommitDoesNoBlockingIoOnTheOwnerThread`. The
existing crash-point tests (`CoordinatorJournalCrashPointsBracketTheAppendedFsync`, the P3 step
crashes) pass unchanged, since the journal write count they hook is the same.

## Counts

Headless, `make`'s environment, from the final tree.

- Focused: `ArenaFreeCellTest`, `IoCaptureTest`, `IoCellsTest`, `IoServiceTest`: 162 tests, repeated
  100 times (`--gtest_repeat=100`): 100 of 100 passes, no failure.
- ThreadSanitizer (`make OBJDIR=build-tsan/ sanitize/engine-thread`, then deleted): `IoCaptureTest`,
  `ArenaFreeCellTest`, `IoCellsTest`, `IoServiceTest`, `StoreSnapshotTest`,
  `SegmentedOpsSpoolRangeTest`, `ArenaManifoldTest`: 269 tests, zero ThreadSanitizer reports.
- Full `xuzz_test`, final tree: 4825 run, 4797 passed, 28 skipped (network, Postgres and opt-in
  profiles, as before), 0 failed. `zigzag_test`: 268 of 268 passed. With P4 alone (before P5)
  `xuzz_test` ran 4818, 4790 passed, 28 skipped, 0 failed. `E2EBinaryOrchestrationTest` passed in
  both runs (its known flake did not fire).
- New tests: 16 `ArenaFreeCellTest`, 3 `IoCellsTest`, 2 `IoServiceTest` (one replaced), 7
  `IoCaptureTest`.

## Not done

- A capture's append request is freed only after the capture's view leaves, and nothing frees it
  then (see "A capture keeps its request").
- `Aborted` fsyncs on the failure paths and journal compaction stay on the owner, as recorded in P3.
- `freeCell` is called only by the I/O reaper; no other arena user (Vortex, VQL, VPL, Vlog) frees
  per cell.
