# Round 1 critique: Xanadu / ZigZag Purist (Vortex I/O M1–M6 code)

Target: 757cd55b (diff 389d21ce..757cd55b). Paths are relative to the repo root. Tally: 1 BLOCKING,
4 MAJOR, 6 MINOR.

Audited and found clean (no finding): the endpoint/capability/request field graph in `io_cells.cpp`
(`attachField` plus `linkFresh` refuses to overwrite a neighbour, so each canonical cell's
`d.io.clone` rank is a chain and no request is a fan-in target on `d.io.endpoint` or
`d.io.capability`); the `d.io.queue` rank; `d.io.outcome`, `d.io.payload`, `d.io.source`,
`d.io.progress` (one private occurrence each, single inbound link); the `d.io.capture` /
`d.io.range` / `d.io.provenance` ranks in `capture_publication.cpp` (receipt joins last; piece and
field chains each have one predecessor); R7 and R8 use in the publisher (it takes only `Store` cell
refs from `cellRefOf`; no arena ref reaches `Store::setLink`); `promoteOrRefuse` refuses
runtime-only and owner-lease cells. `io_actor_journal.cpp`, `lens_adapters.cpp` and
`mesh_lens_channel.cpp` make no cell links in this diff.

## BLOCKING

### 1. A waiter evicts the capture's append-request link; `commit` then refuses forever

**Problem:** `ArenaIoCellStore::mintWaiter` links a fresh occurrence to the request on
`d.io.request`, and `CaptureCoordinator::appendRequest` does the same thing. Both point at the same
target cell. A request cell has one `-d.io.request` slot, so the second `ArenaManifold::link` evicts
the first. `ArenaManifold::link` replaces silently (`zigzag/arena_manifold.cpp:2310-2335`: the
displaced occupant of `to` on `-dir` gets its `+dir` cleared). The I/O code's `linkFresh` guard is
not used on the waiter path. This is the fan-in the 2-degree rule forbids.

**Failure scenario:** Plan §8.2 says the append request is "observed through normal Await".

1. A host calls `appendRequest(capture)`. The capture links `capture -req-> occC -req-> request`.
1. The host calls `IoService::await(request, reg)`. The request is still pending, so `mintWaiter`
   links `waiter -req-> occW -req-> request`.
1. `link(occW, req, POS, request)` displaces `occC` from `request.NEG`, so `occC.POS` is cleared.
1. The append completes. The host calls `commit(capture, outcome)`.
1. `commit` reads `linked(capture, req)`, which is `occC`. It then reads `linked(occC, req)`, which
   is empty. It refuses with `InvalidRequest/2` at `capture_coordinator.cpp:1147-1150`.
1. The capture stays in `AppendRequested`. Its leases, journal hold, `Pending` translation slots and
   `maxPendingCaptures` slot are never released, and the bytes are already in the permascroll with
   no `Committed` record. The retry fails the same way. No test awaits a capture append
   (`grep await( tests/xuzz/io_capture_test.cpp` is empty), and no test reads `d.io.request` back,
   so this is untested. The same eviction hits two waiters on one request (distinct continuations
   pass the `duplicate` filter at `io_service.cpp:476-483`). The first waiter's occurrence loses its
   request link, so the graph lies about waiter-to-request. `retireWaiter`
   (`arena_io_cell_store.cpp:655-660`) then unlinks only what is left.

**Evidence:** `apps/common/xanadu/vortex/arena_io_cell_store.cpp:612-619`;
`apps/common/xanadu/vortex/capture_coordinator.cpp:1092-1100, 1144-1153`;
`apps/common/xanadu/zigzag/arena_manifold.cpp:2310-2335`;
`apps/common/xanadu/vortex/io_service.cpp:476-493`.

**Fix:** Give the waiter its own clone view. Use the existing `attachField` machinery (one
implementation, not the copy in finding 7), with the waiter's occurrence pointing at a view on the
request's `d.io.clone` rank, so `request` is never a direct target on `d.io.request`. The capture
link takes the same route. Use `linkFresh` for every `-> request` link. Add tests: await then commit
on a capture append; two waiters on one request; retire either waiter and check the other's chain.

## MAJOR

### 2. Transient origin ids are process-local but persisted as translation identity

**Problem:** A `Transient` `PayloadRef` carries `source = nextSource.fetch_add(1)` from a
function-local static that restarts at 1 every process (`payload_resolver.cpp:443-446`). The capture
coordinator keys its translation map on `(kind, source, generation, occurrence)`. It rebuilds that
map after restart from `Appended` journal records (`capture_coordinator.cpp:1357-1370`, called from
recovery at 1621). `uncoveredPieces` (642-690) treats a committed slot as "already captured".

**Failure scenario:**

1. Session 1 captures transient source 1 ("AAAA", `[0,4)`). `Appended` is journaled and published.
1. The process restarts. Recovery rebuilds `translations_[(Transient,1,0,0)] = [0,4) -> span`.
1. Session 2 reads a different file. `publishTransientBytes` hands out source 1 again, with bytes
   "BBBB". The user captures `[0,4)`.
1. `uncoveredPieces` finds the committed slot and emits no piece. The capture appends nothing and
   commits a receipt with an empty `d.io.range`.
1. The user's "BBBB" is silently never captured, and the earlier translation is returned as if it
   were BBBB. This is silent wrong-data dedup, and no typed refusal is raised. The Store provenance
   has the same flaw (`capture_publication.cpp:157-163`). It records `source/generation/occurrence`
   as bare numbers with no locator, so after a restart "source 71" names nothing. The plan says
   "Local capture does not invent remote provenance". That does not excuse persisting a session
   counter as if it were identity. `ReadObservationRecord` already carries `locator` and
   `sourceGeneration`, so the information exists and is not used.

**Evidence:** `apps/common/xanadu/vortex/payload_resolver.cpp:440-450`;
`apps/common/xanadu/vortex/capture_coordinator.cpp:642-690, 1357-1370, 1621`;
`apps/common/xanadu/vortex/capture_publication.cpp:157-163`; the test
`tests/xuzz/io_capture_test.cpp:53` uses a fixed Transient ref and never restarts with a changed
source.

**Fix:** Either (a) key non-`Durable` translations by a per-process epoch, so recovered
`Transient`/`Scratch`/`Proxy` slots remain journal history and never match a new session's
selection; or (b) refuse to capture non-`Durable` origins that lack a stable, persisted identity.
For Store provenance, add a locator/generation cell chain (a `d.io.source` value cell) instead of
trusting the numbers. Add a restart test that reuses the same transient id with different bytes.

### 3. The idempotence key `CaptureId` is a journal-local counter but the Store is shared

**Problem:** `publish` returns "already published" when any receipt on `d.io.capture` has an equal
`CaptureId` (`capture_publication.cpp:131-139`). `CaptureId`s are issued from
`journal_.maxCaptureId() + 1` (`capture_coordinator.cpp:456`). The publisher header says the target
may be "the author's tip". The id is unique only within one `capture.journal`.

**Failure scenario:**

1. The journal is replaced or reset (new path, or lost after the 8-bit-sized `journalMaxBytes` cap
   forces rotation by hand), or a second coordinator or device publishes into the same Store.
1. Capture 1 of the new journal reaches `publish`, finds an old receipt with `captureId == 1`, and
   returns it.
1. `commit` records `published = true` and writes `Committed`. The new bytes are in the permascroll
   but no piece or provenance cell was ever minted. The data has no Structure reference.

**Evidence:** `apps/common/xanadu/vortex/capture_publication.cpp:131-139`;
`apps/common/xanadu/vortex/capture_coordinator.cpp:454-457`; `capture_publication.hpp` (header
comment on single writer).

**Fix:** Make the key unique across journals. Examples: a journal-instance id (random, minted once
when the journal is created) joined to the sequence, and recorded as a second scalar on the receipt;
or compare the published piece spans against `appended.span` and refuse on mismatch
(`PersistenceRefusal`) instead of trusting the id. Test: two coordinators with fresh journals over
one Store.

### 4. "Cells are canonical" is not true: request, endpoint and capability state are C++ tables

**Problem:** Plan §3.1's table lists as authoritative graph fields the request's operation,
arguments, state, deadline and progress; the endpoint's kind, generation, state and capability
masks; the capability's rights and limits; the capture's intervals, reservation and phase. It then
says "Logical cell links and tags are canonical; native indices can be discarded and rebuilt from
those fields." The code builds only endpoint, capability, queue, outcome, progress, payload and
source links. `d.io.kind`, `d.io.waiter` and `d.io.lease` are minted but never linked
(`grep IoDimension::Kind|Waiter|Lease` finds only the name table). Request state, operation,
deadline and arguments exist only in `IoService::requests_`; capability rights and consistency masks
live in the authorizer and decoder; the arena stores them only as write-only decimal text (finding
8). The cell tag is in `runtimeTags_` (an incarnation-keyed C++ map in `ArenaManifold`), not "normal
typed cell content". `io_cells.hpp:15-19` and the `Capture` enumerator comment claim the capture's
graph holds "the selected source intervals, map reservation ... and phase". The capture cell has
only decimal counts (`capture_coordinator.cpp:954-973`), and the intervals and map reservation live
only in `PendingCapture` and `translations_`.

**Failure scenario:** The "discard and rebuild native indices from cells" recovery path the plan
promises cannot be taken. After the service state is lost, a request cell has no operation, state or
deadline to rebuild from. Any consumer that reads the graph (a zigzag view, VQL over the arena) sees
a request with no meaning. This is a deferral described as done: a milestone acceptance claim that
is not met.

**Evidence:** `apps/common/xanadu/vortex/io_cells.cpp` (whole file: only Endpoint, Capability,
Occurrence, Queue, Outcome, Clone dims are linked);
`apps/common/xanadu/vortex/io_cells.hpp:15-19, ~45-52`;
`apps/common/xanadu/vortex/capture_coordinator.cpp:954-973`;
`design/vortex-io-implementation-plan.md` §3.1 table and the sentence after it.

**Fix:** Either put the missing fields in the graph (request operation/state/deadline as scalar
cells on `d.io.kind`/`d.io.state`; capability masks as `d.io.lease`/state cells; capture intervals
as a `d.io.range` rank), or amend §3.1 and the header comments to say the C++ tables are
authoritative and the cells are a read-only view, and drop the "rebuildable" claim. Do not leave
both.

### 5. Per-request O(N) rank-tail walks over ranks that never shrink

**Problem:** Every `appendIoRequest` does three `rankTail` walks from the head: the endpoint's clone
rank, the capability's clone rank, and the queue (`io_cells.cpp:107-118, 255-270, 380-392`). Nothing
removes a retired request from any of those ranks. `retireRequest` and `eraseRequestState` erase
only C++ map entries (`io_service.cpp:347-373, 568-590`). `arena_io_cell_store.cpp` has no
request-retirement path at all. `queuedRequests` therefore lists terminal and retired requests too.

**Failure scenario:** One long-lived endpoint and capability serve a stream of N requests (a line
reader, a lens). Submit k walks about 3k links, so the total is O(N^2). The walk is bounded only by
`traversalBound()` (the arena size), so it never refuses, it just slows. `ArenaManifold::compact()`
is refused while any runtime-only cell or owner lease exists (the new `RuntimeOnlyState` check), so
the arena cannot reclaim these cells either.

**Evidence:** `apps/common/xanadu/vortex/io_cells.cpp:107-118, 380-392`;
`apps/common/xanadu/vortex/io_service.cpp:347-373, 568-590`;
`apps/common/xanadu/zigzag/arena_manifold.cpp` (the `holdsLease`/`runtimeOnly_` compact refusal).

**Fix:** Keep a tail handle (cell and incarnation) on the owner side per rank, or link the new view
at the head (clone rank order is not meaningful) so append is O(1). Unlink a request from the queue
rank at retirement: `unlink(prev,+d)` and `unlink(next,-d)`, then relink prev to next so the rank
stays one chain. Add a test that submits and retires 10^4 requests on one endpoint.

## MINOR

### 6. Provenance read-back silently defaults

`readPublication` uses `asInt64(...).value_or(0)` and an unchecked `static_cast<PayloadSourceKind>`
(`capture_publication.cpp:64-76`). A short provenance chain, a non-scalar cell, or an out-of-range
kind reads back as source 0 / `Durable`, with no `PersistenceRefusal`. A damaged store is
indistinguishable from a valid one, and the five fields have meaning only by rank position. Refuse
on a short chain or a non-scalar, and range-check the kind.

### 7. Two implementations of the clone-view attach

`attachField` (`io_cells.cpp:155-180`, transactional, `linkFresh`) and `attachCanonical`
(`arena_io_cell_store.cpp:112-135`, hand-rolled, no `transact`) do the same thing. A drift between
them is how finding 1 will recur. Export one.

### 8. Write-only decimal-text mirrors in cells

`rights` and `consistency` (`arena_io_cell_store.cpp:573-582`), the payload `range` record
(`:201-230`), `progress` fields, and the capture receipt's `captureId`/`appendDigest`/`phase` are
decimal or hex strings that nothing parses back. The authoritative range is the owner lease token,
and the receipt's authority is C++. They are harmless today, but they duplicate truth, and a reader
who trusts them gets a second source that can diverge. AGENTS.md: text needing parsing belongs in
more cells. Either use scalar cells (`makeScalarCell`-style) or drop the mirror.

### 9. The capture cell's `phase` field is stale

It is written once as `"Prepared"` (`capture_coordinator.cpp:962`) and never updated. After
`appendRequest` or `commit` the capture cell still says Prepared. Update it, or link the phase
through `d.io.state`.

### 10. `publish` and `published` rebuild the whole manifold on every call

`published()` and `publishedIds()` each run `store_.rebuildManifold(store_.latest())`
(`capture_publication.cpp:83-87, 99-102`), and `publish` runs it twice more (once at 118, then
inside `published()` at the tail). `completePublications` calls it per recovery. The cost is O(store
ops) per commit. Use `Manifold::advance` or a cached manifold. Partial failure also leaves orphan
cells (acknowledged in the header); a retry mints new ones.

### 11. Test gaps that let findings 1, 2 and 3 through

No test awaits a capture append, none reads back the `d.io.request` link, and none restarts with
reused transient ids or a fresh journal over a shared Store. Add them with the fixes.

VERDICT: NOT CONVERGED
