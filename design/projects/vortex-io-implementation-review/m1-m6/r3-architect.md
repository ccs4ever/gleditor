# Round 3 critique: System Architect (Vortex I/O M1–M6 code)

Target e16ec377. Tally: 0 BLOCKING, 1 MAJOR (R3-1), 1 MINOR.

## Status of earlier findings

- A1 / R2-1 (durable eviction): RESOLVED. Committed slots carry `lastHit`, a one-eighth batch is
  evicted by recency, the journal drops whole captures (`capture_constants.hpp:33-37`,
  `evictCold...` in `capture_coordinator.cpp`). The ceiling is gone; the batch size is a named
  constant with a rationale. R2 minor 1 (no counts) is resolved by `stateCounts()`.
- A2 (commit blocks owner): PARTLY. The commit path no longer does blocking I/O on the owner:
  permascroll sync, the `Appended` and `Committed` fsyncs and the snapshot write all run in worker
  turns (`stageDurableAt`, `capture_coordinator.cpp:308`); the owner only copies the delta. The
  remaining owner-side fsync of the `Append` intent in `appendRequest()`, and owner-side compaction,
  `recover()`, `reconcile()` and `resumePublications()`, are an acceptable residue: honestly
  recorded in `p3-implementation.md` and plan M6 notes, justified by recovery's reliance on "intent
  durable before submit", and off the commit's happy path. Not counted. Note one caveat: owner-side
  `compact()` takes the journal mutex that a worker holds across its fsync
  (`capture_journal.cpp:422,473,537`), so under bound pressure the owner can wait on a worker's
  fsync. That is inside the named compaction residue, but the record should say "may wait on a
  worker", not only "does fsync".

## Lane checks (clean)

- One writer of `ops.nodes`: true. `writeSegmentFile` is now a wrapper over
  `SegmentedOpsSpool::writeSegmentRange` (`segmented_ops_spool.cpp:727-741,757`); the snapshot
  writer calls the same function with `Extend`. No second code path writes segment bytes.
- `Store::save()` is one path: `saveSnapshot` then `SnapshotWriter::write(Buffered)`
  (`store.cpp:4364-4376`); the capture publisher's `save()` is the same two calls with the same
  single retry. No duplicate serialiser: `writeStoreTablesDurably` reuses `serializeStoreTables`.
- Layering: `StoreSnapshot`/`SnapshotWriter` live in `store.hpp` (engine, device-free); the capture
  code consumes them. No `apps/common/ui`, `apps/xuzz` or library-graphics include in the diff.
- `store.tables` rewritten whole per snapshot: not a problem. Per `AGENTS.md` it holds local
  segments, deployment scroll descriptors and one binding; registry, links, editions and annotations
  are Structure operations. It is O(scrolls), not O(ops); a rename plus fsync of a few KiB per
  commit. The operations are the part that scales and those are incremental. Price the owner copy
  instead (minor below).
- 64-request cap: not an arbitrary ceiling. It is the host's `IoBudget::maxRequests`
  (`io_service.cpp:202`), admission control the host sizes, not a coordinator constant; the
  coordinator retiring its own durable-step requests (`capture_coordinator.cpp:1800`) is the correct
  response (it neither raises nor bypasses the budget). Two retired per commit; the append request
  is retained as before.
- DRY: the durable steps reuse `CaptureAppendAdapter`, its key space, `maxStagedAppends` and the
  retired-turn rules (`:308-327`); no second adapter was added.

## MAJOR

### R3-1. `durableOps` conflates "written" with "fsynced": a Fsync commit can overclaim

**Problem.** `SnapshotChannel::durableOps` is advanced after any successful write, Buffered or Fsync
(`store.cpp:4329`). The writer then skips every operation at or below it (`store.cpp:4308-4312`),
and when nothing is left `writeSegmentRange` is never called, so `ops.nodes` is never fsynced by a
`Fsync` write. Only `store.tables` (rename) and the directory are synced (`:4345,4361`). `load()`
has the same flavour (it sets the count to what was read).

**Failure scenario.** Step 2 of a commit is on a worker. Between the owner's `publish()` and the
worker taking the channel mutex, the owner (or recovery) runs a Buffered `Store::save()` of the same
directory; it writes the publication's operations and advances `durableOps` to the end. Step 2 then
finds `endOps() <= durable`, writes no range, skips the fsync of `ops.nodes`, fsyncs the tables and
directory, and the worker appends `Committed`. Power loss: `capture.journal` says Committed,
`ops.nodes` lost its tail, the receipt is absent. `reconcile` trusts `Committed`; the capture is
reported published and is not. Same shape on a commit retry whose publication was already saved
Buffered by `recover()`/`resumePublications()`.

**Evidence.** `store.cpp:4285-4329` (count advance, skip, no fsync on empty range);
`segmented_ops_spool.cpp:790-801` (fsync only inside a write); `store.hpp:215` (Fsync is the default
only for the writer, `save()` passes Buffered).

**Fix.** Track a second atomic, `syncedOps`, advanced only by a Fsync write; in a Fsync write, if
`syncedOps < endOps()`, open the file and `fsync` it (no data write) before the tables. Add a test:
Buffered `save()` then a Fsync `write()` of an unchanged store must record an fsync of `ops.nodes`
(the existing thread-recording seam can count it).

## MINOR

1. `saveSnapshot` copies the whole delta on the owner (`opsSpool.copyOpsAfter`, 64 B/op), so the
   first commit into a fresh directory, or after a refused write, is a memcpy of the whole store on
   the owner (about 64 MB per million operations, tens of ms). Not blocking I/O, so within plan §14,
   but the P3 record claims O(delta) without naming that the first and every retry snapshot is
   O(store); say so.

VERDICT: NOT CONVERGED
