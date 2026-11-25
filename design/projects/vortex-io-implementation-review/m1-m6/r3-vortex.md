# Round 3 critique: Vortex Expert (focused)

Tally: 1 BLOCKING, 0 MAJOR, 1 MINOR. Target `e16ec377` (diff `dbe8e40c..e16ec377`).

## Round-2 MINORs

1. Wrong-thread await: RESOLVED. `requireOwner()` precedes the try (`io_host.cpp:75`); test added.
1. `sourceReachable` comment: RESOLVED (`payload_resolver.hpp` rewritten, code unchanged and
   correct).
1. `parked_.emplace` leak: RESOLVED. The catch retires waiter and VM continuation, then rethrows
   (`io_host.cpp:159-168`). Not tested; no seam exists to inject the failure.

## Checked, no defect

- Worker discipline. Step 1 (`capture_coordinator.cpp:1608-1625`) touches the sink sync and the
  journal. Step 2 (`:1738-1760`) touches an immutable `StoreSnapshot` held by `shared_ptr`, the
  writer and the journal. Neither reads the arena, the Store or the manifold. `publish`,
  `snapshot()` and the receipt-cell mint all run on the owner in `advanceCommit` (`:1651-1700`).
- Request/outcome protocol. `continueCommit` checks the request incarnation, requires `poll` to
  return the supplied outcome and the arena outcome link to match (`:1779-1793`). It never
  resubmits. Refusal is returned as a value. It retires its own request after reading the outcome.
  `retireRequest` is the documented consumer-retire road, and no waiter is registered on these
  requests.
- Retry after a step-2 failure. `pending.durable` is reset before the failure check (`:1801`),
  `publishedInStore` and `receiptCell` persist in memory, so the retry skips publish and the mint.
  It takes a fresh snapshot from the directory's durable count and rewrites. A restart goes through
  `published(key)` by `(journalId, captureId)`. Idempotence holds, and the publication folds through
  `rebuildManifold` as before (the link is written last).
- Eviction. `translations_` is a dedup cache only. Evicting a committed slot touches no arena cell
  and no Store state, and published receipts do not read it. Slots of in-flight captures are
  `Pending`, not `Committed`, so eviction cannot take them. No arena/Store inconsistency found.

## BLOCKING

**B1. `compactJournal` can erase a record a worker has just made durable.** `compactJournal`
(`capture_coordinator.cpp:782-860`, called from `prepare` at `:1206` and `:1219` on the owner) does
`journal_.recover()` and later `journal_.compact(keep)` as two separate lock holds
(`capture_journal.cpp:473` and `:537`). `compact` rewrites the file purely from `keep`, which was
built from the earlier view, and then sets `nextSequence_` from it. The P3 commit steps now append
from workers (`:1608`, `:1738`), so a record can land between the two holds.

Scenario: capture A's step 1 (or step 2) is in flight. The journal is near its bound, so owner
`prepare(B)` fails `hold`, calls `compactJournal`, and `recover()` returns a view without A's
`Appended` (or `Committed`) record. The worker then appends it and reports success. `compact` takes
the lock, writes the image from `keep` (A's Append intent is kept because A is live, the new record
is not) and renames it over the file. The coordinator sets `appendedSequence` and proceeds to
publish and write the snapshot. After a restart the journal holds A's `Append` intent and, after
step 2, a `Committed` record with no `Appended` (or, for step 2, no `Committed`, so recovery
re-publishes). Either way the durable record that "Appended precedes publication" was written to
guarantee is silently gone, and `rebuildFromAppended` never rebuilds the translation. The window is
an fsync wide and the trigger is journal bound pressure with a commit in flight (the same pressure
`DurableCaptureStateIsEvictedSoALongRunPastBothBoundsWorks` creates, but without a concurrent step).
No test combines them.

Fix direction (in my lane's reading, no core file): do recover, filter and compact under one hold of
the journal mutex (a `compactWhere(predicate)` on `CaptureJournal`), or have `compact` refuse when
`nextSequence_` differs from the view it was built from.

## MINOR

1. `sweepDead`/`retire` (`:640-690`) completes a live durable step's spec in the adapter but never
   calls `service_.retireRequest` on `pending.durable->request`, so a capture cell that dies
   mid-step leaves that request state in the service for good (budget leak the P3 note says it was
   avoiding). Retire it there, or let the step's outcome path do so.

VERDICT: NOT CONVERGED (B1)
