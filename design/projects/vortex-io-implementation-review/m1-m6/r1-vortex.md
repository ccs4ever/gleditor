# Round 1 critique: Vortex Expert (Vortex I/O M1–M6 code)

Tally: 1 BLOCKING, 2 MAJOR, 6 MINOR. Target `757cd55b` (diff `389d21ce..757cd55b`).

Reviewed in depth: `VortexVM::stepAwait/parkAwait/resumeAwait/retireAwait`, `IoHost`, `IoService`
(admit, await, pump terminal pass), `BoundedIoScheduler`, `ArenaIoCellStore` mark/release paths, the
`ArenaManifold` incarnation/lease/runtime-only additions, `CapturePublisher::publish`,
`CaptureCoordinator::commit/reconcile/completePublications`, `ActorIoJournal`, the profile gate and
`ProjectionSession` staged paths. Owner/worker discipline holds in what I read: workers (scheduler
`workerLoop`, adapters' `execute`) get only `BackendContext` and `CompletionSink`; the lens commit
batch carries text, not cell refs. Await never submits, so a resume (including the stale-outcome
fall-through in `stepAwait`) cannot resubmit a request.

## BLOCKING

### B1. `ArenaManifold::shadow()` grows `slots_` without `incarnations_`: lookups go wrong

**Problem.** `mintSlot` pushes one `incarnations_` entry per minted cell and `release()` resizes
`incarnations_` to `m.cellCount`, so the vector is assumed parallel to `slots_`. `shadow()` (the
copy-on-write of a base cell) also does `slots_.push_back(copy)` but pushes no incarnation.
`denseOf` returns the dense index for both ephemeral cells and shadowed base cells, and
`incarnationOf` indexes `incarnations_[dense]`.

**Failure scenario.** An arena over a base `Manifold` (the production configuration: Vortex, VQL,
VPL, the projection session) runs one op that writes a base cell (`setContent`, `link`), which
shadows it: `slots_.size()==1`, `incarnations_.size()==0`. The program then mints a request, waiter
or cursor cell. `mintSlot` assigns dense 1 and pushes its incarnation at index 0, so
`incarnationOf(newCell)` reads index 1 of a size-1 vector and returns `nullopt`. Every `ioCellTag`,
`makeRequest`, `mintWaiter`, `stepAwait` ("Await requires a live arena cursor") and `matches()`
check then refuses the cell as stale, so I/O is dead for the rest of the run. Worse, the shadowed
base cell reads `incarnations_[0]`, which belongs to the first minted cell, so
`matches(baseCell, thatIncarnation)` is true: a base cell aliases a runtime cell's identity. With
more shadows than mints the mapping is a permanent skew. After `release()` the resize pads with
incarnation 0. The runtime-only guard in `promoteOrRefuse` (`from.incarnationOf(arena)` then
`isRuntimeOnly`/`ownerLeaseOf`) silently skips cells whose incarnation is `nullopt` or aliased, so
I/O state/leases can be promoted into the Store (the exact thing the new `RuntimeOnlyState` refusal
exists to stop), and `exportPrefix`'s check is bypassed the same way for leases keyed by the wrong
incarnation.

**Evidence.** `apps/common/xanadu/zigzag/arena_manifold.cpp:2449-2455` (`slots_.push_back(copy)` in
`shadow`, no `incarnations_` push); `:2092-2113` (`dense = slots_.size()`,
`incarnations_.push_back`); `:50-54` (`incarnationOf` indexes by dense); `:2758-2759`
(`resize(m.cellCount)`); `:2929-2945` region of promote's runtime-only loop
(`if (incarnation && ...)`). Tests build arenas with no base (`tests/xuzz/io_cells_test.cpp`,
`vortex_io_vm_continuation_test.cpp` harness), so none exercises shadow-then-mint.

**Fix.** Core-file change, needs approval: **P1** — in `shadow()` push a fresh
`nextAllocationIncarnation()` for the shadow slot (identity of the overlaid cell, distinct from its
base ref), with the same pop-on-throw guard as `mintSlot`; assert
`incarnations_.size()==slots_.size()` in `mark()`/`release()`. Add a regression test: arena over a
populated base, shadow a base cell, mint a cell, expect `matches(new, *incarnationOf(new))` and
`!matches(base, incarnationOfNew)`, then mint a request and run Await.

## MAJOR

### M1. `ActorIoJournal` never fsyncs its parent directory: an admission can vanish, recovery Clean

**Problem.** `appendRecord` creates the file with `O_CREAT` and fsyncs only the file.
`CaptureJournal` does `syncParentDirectory()` on creation precisely because "a created file whose
entry was never fsynced can vanish whole"; `ActorIoJournal` omits it.

**Failure scenario.** First durable `Admission` for a fresh journal: `appendRecord` returns success,
the host dispatches the effect, the machine crashes before the directory entry reaches disk. On
restart the file is absent, so `recover()` takes the `ENOENT` path and returns `Clean` with no
records. The DurableEffect contract ("never silently resends on restore", plan §9) is violated: the
intent is re-run although the request was already sent.

**Evidence.** `apps/common/xanadu/vortex/io_actor_journal.cpp:266` (open with `O_CREAT`), `:319-329`
(fsync file only, no directory sync), `:399-402` (ENOENT returns `Clean`); contrast
`capture_journal.cpp:573-581,720-727`.

**Fix.** Share the capture journal's `syncParentDirectory` (promote it to a helper both use) and
call it when `!wroteHeader_` before reporting the first append as durable; treat a failed directory
sync as `BackendFailure` and roll back.

### M2. `VortexVM::stepAwait` lets await-host exceptions escape `step()`/`run()` untyped

**Problem.** `step()` routes Await to `stepAwait` before the `try` that converts exceptions to
`Failed`. `stepAwait` calls `awaitHandler_` unguarded. `IoHost::awaitCell` documents and throws
`invalid_argument`/`runtime_error`/`overflow_error`, and `ArenaIoCellStore::mintWaiter` rethrows
whatever the arena throws, including `ArenaManifold::mintSlot`'s `runtime_error` "exhausted: cannot
allocate beyond allocationLimit\_" and `bad_alloc`.

**Failure scenario.** An arena at its allocation limit executes Await on a pending request:
`mintWaiter` -> `makeWaiterCell` -> `makeCell` throws `runtime_error`, `IoService::await` has no
catch, `IoHost::awaitCell` has none, so the exception propagates out of `VortexVM::run()` to the
caller (UI/scheduler thread). Every other VM failure is a value (`StepDisposition::Failed`, with
`abortOwnedMutationGroups`); the plan's refusal-as-value rule and the fact that the continuation
(`createContinuation` already ran) is left in `issuedContinuations_` (leak, never retired) both
break.

**Evidence.** `vortex_vm.cpp` `step()` (the `stepAwait` call precedes the
`try { result = stepImpl }`), `stepAwait` (`awaitHandler_(request, cursor)` bare);
`io_host.cpp:75-77,105,113,118` (throws), `:87` (continuation created before the throwing calls on
`service_.await`); `arena_io_cell_store.cpp` `mintWaiter` catch-rethrow.

**Fix.** In `stepAwait` wrap the handler call in `try/catch`, retire any continuation issued for
this cursor, and return `fail(...)` (or a `Ready{refusal}` via the host). In `IoHost::awaitCell`
retire the continuation on every throwing path, not only the ones that remember to.

## MINOR

1. `VortexVM::step()` scans all of `parkedAwaits_` on every step of every cursor; with parked awaits
   present this is O(parked) per VM cycle on the hot path. Index by cursor (`vortex_vm.cpp` step
   prologue).
1. `step()` returns `Failed` ("parked cursor incarnation is stale") for a stale parked cursor but
   never retires the parked/issued entry, so both maps keep it until `~IoHost` (`vortex_vm.cpp` step
   prologue; `IoHost::pumpIo` abandons it only when it visits that entry).
1. `parkAwait` uses `getCursorOpcode(cursor).value_or(noCell)`: a sentinel `noCell` stored in
   `ParkedAwait::opcode`; AGENTS.md forbids sentinels outside wire formats. Make `opcode` an
   `optional` or refuse (`vortex_vm.cpp` `parkAwait`).
1. `vortex_vm.hpp` now includes `io_cells.hpp`/`io_protocol.hpp` and carries `ParkedAwait`,
   `WaiterCell`, `OutcomeCell` in the VM class. No frontend/adapter branches exist, so neutrality
   holds behaviourally, but the VM header depends on the I/O layer; confirm with the owner that the
   generic `Await` opcode is allowed to pull these types (alternatively, an opaque handle).
1. `IoService::closeEndpoint` evicts with `closingEndpoints_.erase(begin())` at an inline
   `maxClosingTombstones = 1024`: eviction order is unspecified for an unordered container and may
   drop a still-in-flight closing endpoint, weakening the `Conflict` pre-check; and the bound is an
   unpriced local ceiling (`io_service.cpp:612-615`).
1. `CapturePublisher::publish` does two full `rebuildManifold(latest)` per capture (`publish` and
   `published`) plus an O(captures) receipt-rank walk, so publishing N captures is O(N * store); a
   failure part-way leaves orphan unlinked piece cells that every retry re-mints (append-only
   growth) (`capture_publication.cpp:116-118,138,188`). Idempotence is keyed by `CaptureId` alone: a
   second `publish` with the same id and different pieces silently returns the first publication
   (`:138-142`).
1. `ActorIoJournal` writes the header with two `writeAll` calls before the record
   (`io_actor_journal.cpp:296-302`); a crash inside the header leaves 1..7 bytes, which the
   constructor treats as permanent poison (`:147-154`) and `recover()` reports `Corrupt`, so an
   unadmitted first intent bricks the journal. `CaptureJournal` avoids this by writing header and
   first record in one write with a torn-header repair.
1. `awaitCell` under `ActorTurn` mints a fresh `Rejected` cell per refused Await
   (`io_host.cpp:68-73`); a looping actor grows the arena by one outcome graph per attempt. Cache a
   single profile-refusal outcome per host.

VERDICT: NOT CONVERGED
