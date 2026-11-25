# Round 4 critique: Xanadu / ZigZag Purist (focused)

Target: 720fef61 (diff e16ec377..720fef61). Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of round-3 findings

- r3 MAJOR (compaction erases a worker's record): RESOLVED. `CaptureJournal::compactWhere` takes
  `mutex_` once and does scan (`recoverLocked`), choice (`select`) and rewrite (`compactLocked`)
  inside it (`capture_journal.cpp:539-555`). `compactJournal` builds its keep-list inside the
  callback (`capture_coordinator.cpp:817-826`). A worker's `appendRecord` takes the same mutex, so
  it lands before the scan (and is seen) or after the rename. Non-Clean views refuse rather than
  rewrite. The two tests named in the disposition cover the interleaving.
- r3 MINOR 1 (price P4): RESOLVED (plan M6 notes, per the disposition).
- r3 MINOR 2 (snapshot accumulation): claim VERIFIED, no change needed. `continueCommit` takes a
  fresh snapshot per attempt into a local `optional` (`capture_coordinator.cpp:1683,1702-1704`), and
  it is moved into a `shared_ptr` held only by that step's lambda (`:1785`). A failed step's request
  and lambda are released when it completes, so only one snapshot is live per capture; cost is one
  O(undurable) copy per attempt, not a pile. Not a defect.
- r3 MINOR 3 (evictColdTranslations detail): RESOLVED (`kDetailNothingEvictable = 6`,
  `capture_coordinator.cpp:51`).

## Lane checks on the r3 diff

- Replace via `ops.nodes.tmp` + rename (`segmented_ops_spool.cpp:765-837`): holds append-only-ness
  and R7. The temp is written from `firstOpIndex` with nodes verbatim at their indices, so a
  `MakeCell`'s index (the `CellRef`) is unchanged on reload; rename is atomic, so the reader sees
  the old whole file or the new whole file, never a short one. A failed or crashed write leaves a
  stale `.tmp` that the next Replace truncates (`O_TRUNC`) and that nothing loads. `Extend` still
  verifies the on-disk tail against the snapshot's recorded tail, and the new `BaseMismatch` resets
  (`store.cpp:4360,4388`) route to a whole-store Replace, not an in-place rewrite.
- `store.tables` atomic write for Buffered too (`store.cpp:4232-4250`, `:4428-4431`): pairing with
  the permascroll is unchanged. The tables image still follows the ops write, is gated by the
  sequence check so an older image never lands over a newer one, and carries the same scroll
  descriptors and imported binding from one serializer. Atomicity removes the half-written tables
  window; it does not reorder anything. The superseded-file removal still follows the tables write.
- Shared channel per directory (`channelFor`, `store.cpp:4256-4291`): two `Store`s with different
  histories on one directory cannot corrupt it. Writes serialize on the channel mutex; a store whose
  base differs gets `BaseMismatch` (tail byte-compare) and retakes a whole-store Replace. The
  unlocked `durableOps.store(0)` in `saveSnapshot` (`:4303-4307`) can race a writer's final store
  but can only cause an extra Replace or a refused Extend, never a wrong Extend, since the tail
  check decides.
- No new 2-degree, R8 or R12 issue: the diff adds no rank, link or manifold change, and no view
  movement or activity-store path.

VERDICT: CONVERGED (0 BLOCKING, 0 MAJOR in this lane)
