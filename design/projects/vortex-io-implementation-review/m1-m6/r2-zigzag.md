# Round 2 critique: Xanadu / ZigZag Purist (Vortex I/O M1–M6 code)

Target: dbe8e40c (diff 757cd55b..dbe8e40c). Paths are relative to the repo root. Tally: 0 BLOCKING,
1 MAJOR, 4 MINOR.

## Status of round-1 findings

1. (BLOCKING) waiter evicts the capture's append-request link: RESOLVED. `mintWaiter` and
   `appendRequest` both go through `attachClonedField` (`arena_io_cell_store.cpp:589-596`,
   `capture_coordinator.cpp:1192-1199`), so `request` has no inbound `d.io.request` link at all;
   each owner reaches it owner -req-> occurrence -req-> view, with the view appended to the
   request's `d.io.clone` rank (views posward of the master, one chain, no fan-in). Retirement
   (`detachClonedField` -> `detachFromRank`, `io_cells.cpp:193-213`) unlinks both neighbours and
   relinks prev to next, which is correct for head, middle and tail positions. The commit path reads
   through `canonicalOfField` (`capture_coordinator.cpp:1238`). Test
   `AwaitingTheAppendRequestDoesNotEvictTheCaptureLink` (io_capture_test.cpp:436) awaits twice
   before the outcome, asserts all three owners resolve to the request, asserts
   `linked(request, d.io.request, NEG)` is empty, retires one waiter, checks the other, then
   commits. It proves the claim.
1. (MAJOR) process-local origin ids persisted as identity: RESOLVED for the translation map and
   pins. `rebuildFromAppended` (`capture_coordinator.cpp:1470`) and `pinFromRecord` (`:1521`) skip
   every non-`Durable` origin, so a restarted process's source 1 cannot match an old slot or pin.
   Tests `ARestartedProcessReusingATransientIdIsNotAliasedToTheOld` and
   `AnOpenIntentOnATransientSourceDoesNotPinTheNextSession` cover it. The Store provenance still
   records the bare process-local numbers; see minor 1. Rejecting the locator is acceptable: a
   `PayloadRange` carries none, and the plan now says so.
1. (MAJOR) `CaptureId` is journal-local: RESOLVED. `capture.journal` v3 has an `Identity` first
   record with a random 64-bit id (`capture_journal.cpp:346-357`). The receipt's `d.io.provenance`
   rank gets a scalar cell holding the journal id, linked before the seal (`capture_publication.cpp`
   publish, after the piece chains and before the `d.io.capture` link). That is a cell on a
   dimension, with no packed bits, and one inbound link per scalar, so 2-degree and R7 hold (every
   `setLink` subject is a Store `cellRefOf`). The idempotence key is
   `PublicationKey{journalId, captureId}`. The same key with other pieces is a `PersistenceRefusal`
   (detail 14). Test `TwoJournalsOverOneStoreNeverTakeEachOthersReceipt` plus
   `PublishingOneKeyWithOtherPiecesRefusesInsteadOfSkipping`. Survival across `Store::save` and
   reload is only partly proven; see minor 2.
1. (MAJOR) "cells are canonical" not true: PARTLY. The §3.1 paragraph (plan lines 100-122) is honest
   about the departure and its price: `CellRequest` is cell-free because workers never see cells
   (§1.1), capability authority "is never reconstructed" (the table's own sidecar column), and
   recoverable capture state is the fsynced journal because the arena is runtime-only. The graph
   cannot be rebuilt from cells, and the withdrawal says so. But the plan still contradicts itself
   in four other places; see MAJOR 1.
1. (MAJOR) rank-tail O(N^2): RESOLVED. `eraseRequestState` calls `cells_.reapRequest`
   (`io_service.cpp:378`) once the outcome is linked, and `reapIoRequest` removes the request from
   the queue and from the endpoint and capability clone ranks with neighbours relinked
   (`io_cells.cpp:216-221`). Test `reapedRequestsLeaveTheRanksEverySubmitWalks` checks a middle
   reap, chain integrity, a double reap, and 10^4 submit+reap leaving the rank lengths unchanged.

Dispositions judged: finding 8 rejection is acceptable (the mirrors are runtime-only, non-persisted,
and now declared non-authoritative; dropping them is optional). Finding 10 partial is acceptable
(`Manifold::advance` is a core API, and the rebuild runs at commit, which the plan already records
as an owner-thread deviation from §14, A2/P3).

## Round-1 fix diff: new violations

No new 2-degree, R7, R8 or R12 violation found. Every `-> request` link goes through
`attachClonedField`. Reaping touches only ephemeral arena links and never `Store::setLink`.
`reapIoRequest` leaves a reaped request's own occurrence/view dangling by design (the outcome link
is kept, per its comment); nothing reads `requestEndpoint`/`requestCapability` on a request after
its state is erased (grep: no caller outside `io_cells.cpp`).

## BLOCKING

None.

## MAJOR

### 1. The plan still asserts the withdrawn claim in four places

**Problem:** The §3.1 reconciliation paragraph withdraws "logical links and tags are canonical;
native indices can be rebuilt from them", but §2 and §3 still state it as contract. The fix for
round-1 finding 4 was "amend, do not leave both"; both are now left.

**Failure scenario:** M7 and M8 implementers, or a later reviewer, read §2's gate row (the
acceptance contract) and §3.2 (line 238), implement or test "rebuild native sidecars from cells",
and find no fields to rebuild from. The acceptance gate "M0 topology tests" cannot be met as
written.

**Evidence:** `design/vortex-io-implementation-plan.md:58` ("Managed graph authoritative; private
service metadata only accelerates"); `:90-97` (the "Authoritative graph fields" table, including
"Rebuildable cache only" for Outcome/Progress, and the Request row's "operation, immutable arguments
... state ... deadline"); `:238` ("native sidecars may index them but must be
rebuildable/validated"); `:405` ("mint methods create authoritative managed
request/outcome/endpoint/ capability graph values").

**Fix:** Edit those four spots (at minimum retitle the table column "Target graph fields (not met;
see reconciliation below)", and change line 58 to "Cells are the inspectable owner-thread view;
native tables are authoritative") so the document has one story.

## MINOR

1. Provenance for non-`Durable` origins records the process-local source/generation/occurrence
   numbers beside the journal id (`capture_publication.cpp` publish; plan notes call them
   "diagnostics"). Two sessions under one journal both write "Transient, source 1", so the pair
   `(journalId, source)` still does not identify a source. Write the kind only (or a clear "no
   identity" marker) for non-`Durable` origins so no reader mistakes the number for identity.
1. `PublicationRoundTripsThroughStoreSaveAndReload` (io_capture_test.cpp:2053) reads back with the
   legacy `published(CaptureId)` and never asserts that the journal id survives `Store::save` and
   reload. Assert `published(PublicationKey{journalId, 1})` after reopen.
1. The legacy `published(CaptureId)` and `publishedIds()` ignore the journal and return the first
   match; they are the exact hazard finding 3 removed. Restrict them to tests/diagnostics or delete
   them.
1. Reaping unlinks ranks but never frees the request, occurrence and view cells. They stay
   runtime-only, and `compact()` refuses with any runtime-only cell, so arena cell count still grows
   per request (rank walks are fixed, memory is not). Say so in `io_cells.hpp`'s reap comment or
   price it.

VERDICT: NOT CONVERGED
