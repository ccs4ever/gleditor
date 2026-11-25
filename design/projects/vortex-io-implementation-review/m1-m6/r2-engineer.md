# Round 2 critique: Software Engineer (Vortex I/O M1–M6 code)

Target: dbe8e40c (diff 757cd55b..dbe8e40c). Tally: 0 BLOCKING, 2 MAJOR, 5 MINOR. Read-only review;
nothing was built or run. Reviewed: `framed_log.{hpp,cpp}`, `io_actor_journal.cpp` (v2),
`capture_journal.cpp` (v3, Identity), the capture coordinator's recover, reconcile, compaction and
reaping paths, `capture_publication.cpp`, the clone-view waiter topology (`io_cells.cpp`,
`arena_io_cell_store.cpp`), `payload_resolver.cpp`, the `io_service.cpp` and `io_host.cpp` diffs,
and, newly, `vortex_streams.cpp` and `mesh_lens_channel.cpp`.

## Round 1 status

- **E1 (pumped progress erases byte limits): RESOLVED.** Limits live in `SinkState::limits`, erased
  with the request (`io_service.cpp:253-256`, `:945-952`). The regression test
  `PumpedProgressDoesNotEraseTheRequestsByteBounds` (`io_service_test.cpp:431`) sends progress,
  pumps it, then delivers a payload candidate inside the bound, and asserts the outcome is not
  `BudgetExceeded`. It exercises the failing interleaving.
- **E2 (durable origin keyed by start, cached forever): RESOLVED.** The longer read replaces a
  shorter chunk, a shorter read is served from the longer, and the registry is weak with an
  amortised sweep (`payload_resolver.cpp:181-195`). The test asserts all three resolves succeed, the
  bytes read back, and `liveSources()` is 0 after release. Side effect: see m1.
- **E3 (actor journal treats an oversized length as a tear): RESOLVED.** Version 2 has a per-record
  FNV checksum, a gapless sequence and `tornTailPlausible` (`io_actor_journal.cpp:103-130`,
  `framed_log.cpp:144-176`). A mid-file length that is large enough to run past EOF is checked
  against the kind, sequence and `argsLen`, and the scan returns corrupt rather than cutting. The
  tests `CorruptMidFileLengthRefusesInsteadOfCuttingRecords` and
  `ChecksumRefusesAFlippedIntentIdentity` match the scenario.
- **E4 (no directory fsync, split header/record write): RESOLVED.** `FramedLog::append` writes the
  header and first record in one `write`, fsyncs the directory when creating, and rolls back when
  that fails (`framed_log.cpp:312-378`). `repairTornHeader` handles a header remnant.

Framing, checksum and torn-tail logic in `framed_log.cpp` and the capture journal's Identity record
(first record and only the first, `capture_journal.cpp:325-329`; header-only file handled) look
sound. Compaction itself is atomic in the intended way: the replacement is written, fsynced, renamed
over the journal, then the directory is synced, and a stale temp is deleted at open
(`framed_log.cpp:150-157`, `:407-441`). The one flaw is in what the callers do when the last step
fails (M1).

## BLOCKING

None.

## MAJOR

### M1. A failed directory sync after a compaction rename leaves `nextSequence_` stale: a gap

**Problem.** `FramedLog::replaceRecords` renames the new image over the journal and updates
`writtenBytes_`, then returns failure if `syncParentDirectory` fails (`framed_log.cpp:437-440`).
Both callers treat any failure as "nothing changed" and return before resetting their in-memory
sequence: `CaptureJournal::compact` (`capture_journal.cpp:551` returns before `:553`) and
`ActorIoJournal::compactTo` (`io_actor_journal.cpp:278-281`). The file now holds records numbered
1..N', but the object still believes the next sequence is the old, larger value.

**Failure scenario.**

1. The journal holds 40 records. `reserve()` or `prepare()` compacts it down to 5. The rename
   succeeds and the directory fsync returns EIO.
1. The compaction returns an error. `prepare()` falls through and refuses the capture, or the actor
   `reserve()` returns the original `BudgetExceeded`. Neither is marked poisoned.
1. A later append (the next `Committed`, a `Receipt`, or a release) is framed with `nextSequence_` =
   41 on top of a file whose last record is 5.
1. On the next open, `scanImage` rejects the record (`decoded->sequence != ordinal + 1`). `open()`
   poisons the journal, and `recover()` returns `Corrupt` for the whole file, including the durable
   `Appended` and `Admission` evidence the compaction had kept.
1. Recovery is refused permanently. The capture journal's pins and the actor journal's `Unknown`
   admissions cannot be classified.

The in-process `recover()` has the same effect, because it also parses the image and fails on the
gap.

**Evidence.** `framed_log.cpp:429-440`; `capture_journal.cpp:551-553`;
`io_actor_journal.cpp:278-281`. The tests `CreationWithAnUnsyncableDirectoryIsNotDurable` and the
compaction tests cover only the creation path, not a post-rename directory failure.

**Fix.** Make "renamed but not directory-synced" its own state: poison the journal, or have
`replaceRecords` report that the rename happened. Callers then update `nextSequence_` and
`identityWritten_` from the image they wrote regardless of the sync result, and still surface the
refusal. Add a test with an unsyncable directory that runs a compaction followed by an append, then
reopens.

### M2. `MeshLensChannel` frames values with `|` and splits on every `|`: names truncated

**Problem.** `LENS_REQ|node|cellName|proxyRef` and `LENS_RESP|node|cellName|content` are built by
concatenation and read back with `split(payload, '|')`, using `parts[3]` as the whole content
(`mesh_lens_channel.cpp:56-58`, `:93-96`, `:151-158`, `:160-161`). Nothing escapes or
length-prefixes the fields.

**Failure scenario.**

1. A remote cell holds the text `a|b`, which is ordinary text in a spreadsheet-like slice. The peer
   answers `LENS_RESP|nodeA|cell|a|b`.
1. `pollIncoming` splits on `|`, keeps `parts[3]` = `a`, and marks the proxy `Active` with
   `content = "a"`. The rest of the value is dropped silently and the proxy reports success.
1. Likewise a cell name `x|y` is requested as `LENS_REQ|node|x|y|7`. `servePeerRequests` takes
   `cellName = "x"` and serves a different cell. The response is keyed `node:x` and activates any
   waiting proxy for `x`.

This is silent data corruption on a reachable input, on a path whose contract is "immutable value
messages".

**Evidence.** `mesh_lens_channel.cpp:8-16` (split), `:56-58`, `:93-96`, `:151-162`. No test in
`tests/xuzz/mesh_lens_channel_test.cpp` uses a delimiter inside a name or value.

**Fix.** Length-prefix the fields (or reuse the bencode/envelope encoder `spool_mesh.hpp` already
has) and refuse a message that does not parse exactly. Add a round-trip test with `|` in both the
name and the value.

## MINOR

### m1. A durable `PayloadRange` held by value cannot be retained; `sourceReachable` always true

After E2, `retain()` of a `Durable` range returns `Unresolved` once no lease or owner token keeps
the chunk alive (`payload_resolver.cpp:111-125`), and nothing re-reads the permascroll there. Yet
`sourceReachable` returns `true` for every durable origin (`payload_resolver.cpp:441`), and its
header comment says "a durable origin always". A caller that stores a durable range (for example a
capture selection) and retains it later gets `Unresolved`, unlike before the change. Either document
and test the lease-only contract on `retain`, or make `sourceReachable` agree.

### m2. A checkpoint that compacts a full journal can grow it past `maxBytes`

`ActorIoJournal::checkpoint` calls `compactTo(&record)` when the journal is full
(`io_actor_journal.cpp:331-338`). If the floor releases nothing, the rewrite is the old records plus
the checkpoint. `replaceRecords` does no size check, unlike `CaptureJournal::compact`
(`capture_journal.cpp:545-549`). The cap is exceeded by one record and the call reports success.
Check the rewritten size against `maxBytes` and return `BudgetExceeded`.

### m3. `reapIoRequest`, `detachClonedField`, `detachFromRank` are `noexcept` over throwing calls

`ArenaManifold::link` can throw (shadow allocation, link-run limit, incarnation exhaustion:
`arena_manifold.cpp:2326-2346`, `applyLinkWrites`). `detachFromRank` unlinks both sides and then
re-links the neighbours (`io_cells.cpp:193-202`). A throw from the re-link inside a `noexcept`
function calls `std::terminate`, in `IoService::eraseRequestState` on the owner thread. It would
first leave the rank cut in two if it were caught. Catch inside and log, or do the re-link first.

### m4. `CaptureAppendAdapter::complete` deferral can erase the next capture's receipt

With more than one worker, a retired capture's deferred `complete(key)` sits in `completeAfterTurn_`
(`capture_coordinator.cpp:308-315`). A new capture that reuses the raw `uint32` key can start its
own turn, set `receipts_` and `executed_`, and then the first turn's `endTurn()` erases both
(`:391-397`), so `wasExecuted` reads false for a capture that did execute and can classify it as a
certain abort without a pin. This is a narrow window and only matters with multiple workers. Key the
adapter tables by `(cell, incarnation)`.

### m5. `MeshLensChannel` never removes a timed-out request from `pendingRequests_`

`checkTimeouts` marks the proxy `Disconnected` but leaves the entry
(`mesh_lens_channel.cpp:124-137`). A late `LENS_RESP` then turns a `Disconnected` proxy `Active`
again (`:99-105`), and entries for peers that never answer stay for the object's lifetime. Erase the
entry on timeout, or bound the map.

## Not reached, nothing found

`vortex_streams.cpp` (stream/iterator opcodes: copy-on-write of shared stream vectors, work
accounting before mutation, cursor ownership) had no defect I could show a scenario for.

VERDICT: NOT CONVERGED
