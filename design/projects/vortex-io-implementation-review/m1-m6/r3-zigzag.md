# Round 3 critique: Xanadu / ZigZag Purist (focused)

Target: e16ec377 (diff dbe8e40c..e16ec377). Tally: 0 BLOCKING, 1 MAJOR (cross-lane), 3 MINOR.

## Status of round-2 findings

- r2 MAJOR 1 (plan asserts withdrawn claim): RESOLVED. Plan §2 gate row and Authority column (lines
  54-57), the §3.1 table header "Target graph fields (not met; see the reconciliation below)" and
  "the cells are a view of it" (lines 90-97) now agree with the reconciliation paragraph (line 103).
- r2 MINOR 1 (provenance writes process-local numbers): still skipped; stays MINOR, unchanged
  (`capture_publication.cpp:215-221`). Not re-raised.
- r2 MINOR 2, 3 (reload by journal key; legacy `published(CaptureId)`): RESOLVED (tests use
  `PublicationKey`, accessor deleted).
- r2 MINOR 4 / P4 (reaped cells never freed): see judgement below. Residue, MINOR.

## Lane questions

- Eviction versus published receipts: sound. A receipt holds permascroll spans and the provenance
  chain in the Store; the translation map is a dedup cache over journal-proven spans (plan §8) and
  publication never reads it. Eviction removes nothing a receipt addresses.
- Re-capture after eviction: the range is appended again as a new permascroll span, so the Store
  holds two receipts over two distinct spans with the same source origin. No content address is
  duplicated and no provenance contradicts: two originations of equal bytes, each honestly recorded.
  This is acceptable (the price is storage, not identity). Keys cannot collide: the journal's
  Identity record carries the max `CaptureId` through compaction (`capture_journal.cpp:368`,
  coordinator `:582`), so ids never repeat.
- P3 publish-before-durable: publish runs (`capture_coordinator.cpp:1637-1650`) only after step 1
  made `Appended` durable, and `Appended` makes publication owed (recovery publishes at `:2267`). A
  failed step 2 leaves the capture retriable (`publishedInStore` stays true), never aborted, so a
  reader of `rebuildManifold` can see published Structure ops only for a capture that will complete.
  These are ordinary appended operations, not view movement; no R8 issue.
- Append-only and R7 across `Extend`: `copyOpsAfter` copies nodes verbatim at their indices
  (`segmented_ops_spool.cpp:821-826`), the writer checks the on-disk tail equals the snapshot's
  `baseTail_` byte for byte and refuses otherwise (`store.cpp:4251-4256`, writer checks
  `:4281-4300`). Indices are never renumbered, so every `CellRef` (a MakeCell index) is preserved.
  Holds.
- MeshLensChannel bencode: the frames are the remote-lens wire encoding; lens names and values are
  carried as bytes on the wire and the program interface stays cell-only. Wire serialisation is the
  permitted boundary. No finding.

## MAJOR

### 1. Journal compaction can drop a record an in-flight worker step just wrote (cross-lane)

**Problem:** `compactJournal` builds its keep-list from `journal_.recover()` (lock taken and
released, `capture_coordinator.cpp:783`) and later calls `journal_.compact(keep)` (second lock,
`capture_journal.cpp:535-537`). P3 moved the `Appended` and `Committed` appends onto workers
(`:1600-1625`, `:1706-1725`), and `prepare` for another capture compacts on the owner whenever the
journal hold fails (`:1206`, `:1219`).

**Failure scenario:** capture X is in step 1 on a worker; the owner prepares capture Y, the journal
is full, `compactJournal` calls `recover()`; the worker then fsyncs X's `Appended`; the owner calls
`compact(keep)` with a list lacking that record and `replaceRecords` renames the new image over it.
X's bytes are in the permascroll, its `Appended` is gone, its `Intent` remains. X publishes and
`Committed` is appended; a crash then recovers an open intent with no `Appended`, classified as
uncertain and pinned, and the receipt is in the Store with no journal proof (or a re-append
duplicates bytes). If the lost record is `Committed`, recovery republishes harmlessly, so the damage
is the step-1 window.

**Evidence:** `capture_coordinator.cpp:783,1206,1219`; `capture_journal.cpp:473,535-537`. No test
runs a compaction concurrent with a step; `TwoCommitsInFlightTogetherLeaveBothDurable` does not
compact.

**Fix:** make recover-and-compact one critical section in the journal (a `compactKeeping(predicate)`
that scans under the lock), or refuse compaction while any durable step is outstanding.

## MINOR

1. P4: judged a MINOR residue, not escalated. Growth is three runtime-only cells per request in an
   ephemeral arena, never touching a Store, and the rank walks are bounded (the O(N^2) was the real
   defect, fixed). It does become an unbounded-growth item for a process that serves requests
   without end, so the plan should price it (cells per request, bytes) next to P4 and say a
   request-scoped mark is the owed core change. Escalate only if M7 hosts long-lived services.
1. `CaptureCoordinator::continueCommit` takes a snapshot on the owner each attempt (`:1654-1658`)
   that copies all undurable ops; after a long outage of step 2 retries the snapshot grows without
   bound. Cap or document the price.
1. `evictColdTranslations` returns 0 when fewer than `wanted` slots are eligible (`:738`) while
   pinned or pending slots fill the map, so prepare refuses rather than evicting what is eligible.
   Intentional per the disposition; add a stable detail so the refusal names this cause.

VERDICT: NOT CONVERGED (one MAJOR, the compaction/worker race)
