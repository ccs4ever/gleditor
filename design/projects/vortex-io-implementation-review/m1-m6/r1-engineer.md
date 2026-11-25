# Round 1 critique: Software Engineer (Vortex I/O M1–M6 code)

Target: 757cd55b. Tally: 0 BLOCKING, 4 MAJOR, 6 MINOR. Read-only review; nothing was built or run.
Reviewed in depth: `io_service.cpp`, `io_scheduler.cpp`, `payload_resolver.cpp`,
`io_actor_journal.cpp`, `capture_journal.cpp`, `capture_publication.cpp`, the capture
coordinator's append, commit, retire and adapter paths, `file_source_adapter.cpp`,
`file_endpoint_adapter.cpp`, `lens_adapters.cpp`, `io_host.cpp`, and the publishTerminal and
mintOpenedEndpoint parts of `arena_io_cell_store.cpp`. Not read in depth: `vortex_streams.cpp`,
`mesh_lens_channel.cpp`, and capture recovery and reconcile (`capture_coordinator.cpp:1335-1876`).

Checked and found sound: scheduler lock discipline and slot lifecycle, the release-ack ordering
that gates reservation release, the capture journal's fsync ordering (bytes, then `Appended`, then
Store save, then `Committed`), capture journal torn-tail plausibility, and the fd guards.

## BLOCKING

None.

## MAJOR

### E1. A pumped progress event erases the request's byte limits, so a later payload fails BudgetExceeded

**Problem.** `IoService::admit` stores `completionLimit` and `payloadLimit` in the same
`SinkState::Event` that `progress()` fills. `pump()` moves the whole event out and erases the map
entry. `terminalCandidate` then does `events.find(token)`, finds nothing, and falls back to limits of
`0`.

**Failure scenario.**
1. A read is admitted with `payloadBoundBytes = 1 MiB`.
2. The worker reports progress. `scheduler_->pump` delivers it to `IoService::progress`, and
   `IoService::pump` drains it and erases the event, limits included.
3. The worker then calls `terminalCandidate(Complete{range, ...}, lease)`. `completionLimit` and
   `payloadLimit` are 0, so `payload->range.length > payloadLimit` is true.
4. The outcome is rewritten to `Failed{BudgetExceeded}` and the payload is dropped.

Any request that reports progress and is pumped before it completes can never deliver a non-empty
payload or `staged` bytes.

**Evidence.**
- `io_service.cpp:247-250` (limits stored in the event).
- `io_service.cpp:630-637` (`event = std::move(...)`, then `events.erase(found)`).
- `io_service.cpp:899-911` (limits read back, 0 when missing).
- The same bug applies to a `releaseAcknowledged` event that is pumped before the candidate.

The bug is latent today: no shipped adapter calls `sink.progress` (grep finds only
`io_scheduler.cpp:256`). The sink contract allows it, so the first adapter that emits progress hits
it.

**Test gap.** `io_service_test.cpp:402-415` (`ProgressCoalescesWhile...`) pumps progress and then
completes with `Complete{{}, {}, {}}`, which has no payload. The one scenario that exposes the bug
is untested.

**Fix.** Keep the limits in `RequestState` or in a separate map that is not consumed by the event
drain. Alternatively, have `pump` leave the limit fields in the map entry and move out only the
payload fields. Add a test that sends progress, pumps, and then delivers a payload candidate.

### E2. Durable payload origins are keyed by `start` only, and cached forever

**Problem.** `resolveDurable` builds `origin = {Durable, scroll, 0, span.start}`, so the length is not
part of the identity. `adopt()` reuses whatever chunk was registered first under that origin. A second
resolve at the same start with a different length returns `Conflict`, even though the bytes are valid.
`sources` is never evicted except by `retireSource`, and every durable chunk is a full copy of the
span.

**Failure scenario.**
1. A caller resolves a prefix span `(scroll 0, start 100, len 10)`.
2. The same caller, or a capture, resolves the whole span `(start 100, len 50)`. This returns
   `Conflict`, or, if the longer span was adopted first, silently serves shorter semantics to later
   callers.
3. A capture path that resolves many distinct spans copies each one into `sources` permanently. This
   breaks the owner's "memory bounded, no ceilings by accident" rule.

**Evidence.**
- `payload_resolver.cpp:222-233` (origin without length; `Conflict` on size mismatch).
- `payload_resolver.cpp:162-169` (`adopt` keeps the first chunk).
- `payload_resolver.cpp:77-79` (the comment says "stay resolvable for the resolver's lifetime").
- `payload_resolver_test.cpp:91-114` covers only one span per start, so it passes.

**Fix.** Key the durable origin by `(scroll, start, length)`, or store a per-`(scroll, start)` chunk
that grows to cover the longest span and slice from it. Make `sources` weak or refcount-evicting the
way `transientSources` is, or document and price a bound.

### E3. `ActorIoJournal` treats any oversized length as a tear and then truncates away the valid records after it

**Problem.** `journalPrefixEnd` breaks out of the scan as a "tolerated tear" whenever
`bodyLen > bytes.size() - cursor - 4`, with no check that the partial bytes are a plausible prefix of
a real record. The constructor's `repairTornTail` then `ftruncate`s the file to that boundary. The
record format has no checksum. `CaptureJournal` fixed exactly this with `tornTailPlausible`, and the
actor journal did not get the fix.

**Failure scenario.**
1. A bit flip or garbage lands in the length prefix of a mid-file record, for example
   `0x7fffffff`.
2. The next process start opens the journal. The scan stops at that record as a tear, and
   `repairTornTail` truncates everything from that point on.
3. Later `Admission` and `Receipt` records are destroyed, and `recover()` reports `Clean`. A durable
   admission is forgotten, so the request can be resubmitted. This violates "automatic resume never
   resubmits, no silent retry".

**Evidence.** `io_actor_journal.cpp:90-96` (the tear rule), `:197-211` (truncate), and `:93`,
which handles only too-small lengths.

**Fix.** Port the capture journal's plausibility test: the kind is valid, the announced length is
consistent with the kind and `argsLen`, and the sequence is the expected one. Prefer adding a
per-record checksum, which needs a bumped `kActorJournalVersion`. Refuse as `Corrupt` instead of
truncating when the tail is not provably a tear.

### E4. `ActorIoJournal` never fsyncs its parent directory on creation and splits header and first record into two writes

**Problem.** `CaptureJournal` writes the header and first record in a single `write` and calls
`syncParentDirectory()` after the first fsync. `ActorIoJournal` does neither. It issues the header and
the record as separate `writeAll` calls, and it has no directory fsync.

**Failure scenario.**
1. The first `Admission` is created and fsynced, and the caller treats it as durable.
2. Power is lost before the directory entry is persisted. This is legal for ext4 and xfs, which do
   not guarantee that an fsync of a new file persists its name.
3. The journal file is gone. `recover()` hits `ENOENT` and returns `Clean`
   (`io_actor_journal.cpp:399-403`) over an admission that was supposed to be durable. The request
   may be resubmitted.

A crash after the header write but before the record write can also leave 1 to 7 bytes of header.
The constructor then poisons the journal permanently, and later appends refuse
(`io_actor_journal.cpp:147-154`). The test `PartialHeaderFilePoisonsInsteadOfAppending` pins this
behaviour rather than repairing it.

**Evidence.** `io_actor_journal.cpp:296-302` (two writes), `:319-330` (no directory sync), against
`capture_journal.cpp:652-665` and `:722-726`.

**Fix.** Reuse the capture journal's creation protocol: one write containing the header and the
first record, then fsync the parent directory, plus `repairTornHeader` on reopen. This is also a DRY
point: both journals duplicate the scan, repair and append skeleton.

## MINOR

### m1. `IoService::admit` leaves file descriptors and registry entries behind if the open outcome never reaches `openSettled`

If `adapter->publish` or `cells_.mintOpenedEndpoint` throws, the catch block at `io_service.cpp:754-771`
sets a Failed outcome and never calls `state.adapter->openSettled(...)`. `FileEndpointAdapter` has
already registered the descriptor in `files_` and `pendingOpens_` on the worker
(`file_endpoint_adapter.cpp:195-197`). Nothing can ever address that file again, and the fd stays open
until the adapter dies. The same happens for an open request whose state is destroyed at shutdown.
Fix: call `openSettled(token, std::nullopt)` in the catch path, and have `~FileEndpointAdapter` or a
`reap` hook close pending opens.

### m2. `CaptureAppendAdapter::complete()` can run while the worker is still inside `execute`

`retire()` calls `adapter_.complete(key)` (`capture_coordinator.cpp:553`), which erases `receipts_`
and `executed_`. `sweepDead` retires a capture whose cell died while its append is still running.
The worker then does `receipts_[key] = span` and `executed_.insert(key)` after the erase
(`:406`, `:416`). The entries leak, and a later capture whose spec cell reuses the same raw `uint32`
ref sees a stale `executed_` flag. `wasExecuted` becomes true for a capture that was cancelled before
start, which causes an unnecessary uncertain pin. The flag errs in the safe direction, so this is
only a leak and a false pin. Fix: key by `(cell, incarnation)`, or defer `complete` until the
request's terminal.

### m3. `closingEndpoints_` tombstone eviction removes `begin()`, not the oldest

`io_service.cpp:613-615` erases `closingEndpoints_.begin()`. For an ordered map that is the smallest
cell ref, and for an unordered map an arbitrary bucket. A live in-flight close can be evicted while
an old finished tombstone stays. The comment says "eviction only weakens the pre-check". That is
true for a finished close, but a live one is also a victim and a second concurrent close is then
admitted. Fix: keep an insertion-ordered deque and evict only completed tombstones.

### m4. File reads can read and allocate `payloadBound + 256` bytes and then fail

The backend byte budget is `completionBoundBytes + payloadBoundBytes` (`io_scheduler.cpp:131`,
`:329`). `FileSourceAdapter::execute` uses that as `maxBytes` for the read
(`file_source_adapter.cpp:400-405`). The service then rejects any payload larger than
`payloadBoundBytes` (`io_service.cpp:909-911`). A read with `range.length >= payloadBound + 1` on a
large file therefore does the whole read, allocates, publishes a transient chunk, and finally returns
`BudgetExceeded`, instead of clamping to the payload bound and returning a short read or `Partial`.
Fix: pass the payload bound separately in `BackendBudget`, or clamp at `payloadBoundBytes`.

### m5. Capture journal rejects a crash tear made of zero bytes

`scanJournalImage` returns `corrupt` for `bodyLen < kFixedBodyBytes` (`capture_journal.cpp:404-408`).
A zero-extended tail is a common crash artifact on filesystems that update the size before the data.
The result is a journal that poisons permanently instead of being cut. Treat an all-zero remainder as
a tear.

### m6. `syncParentDirectory` ignores failure, and `FileEndpointAdapter` opens without `O_CLOEXEC`

`capture_journal.cpp:573-581` silently returns if the directory cannot be opened or fsynced, yet
`appendRecord` has just declared the record durable. `file_endpoint_adapter.cpp:181` opens the target
without `O_CLOEXEC`, so a child process leaks the fd. A FIFO or device locator also blocks the worker
in `open()` with no way to cancel it, and `~IoService` then hangs in `scheduler_.reset()`. Fix:
surface the directory-sync failure as `PersistenceRefusal`, add `O_CLOEXEC | O_NONBLOCK` on open, and
verify `S_ISREG`.

VERDICT: NOT CONVERGED
