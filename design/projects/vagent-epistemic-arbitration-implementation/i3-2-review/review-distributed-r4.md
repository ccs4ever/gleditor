# I3-2 design review, round 4: distributed-systems expert (persona distributed)

Scope: `i3-2-design.md` revision 4 (`d710e7a3`), `decisions.md` sections 1 (R3-1 to R3-13) and 3, my
round-3 report `review-distributed-r3.md`, and the diff `1140853d..d710e7a3` for sections 3.2, 4,
5.2 to 5.4, 6.3 to 6.7. Code at HEAD `d710e7a3`. This is a confirmation round: I read the text and
the code it cites and ran no new experiment. Nothing was edited except this file.

## Earlier findings

| Finding                                  | Status   | Where and why                                                                                                                                                                                                                                                                                             |
| ---------------------------------------- | -------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1 reset floor from every passing read   | Resolved | R3-6. 5.4 keeps a bounded in-flight multiset under the ingress mutex, snapshotted by the call; `admittedSeq` is deleted; 6.4 caps the floor at `lastSeq + healthSequenceGap`; 6.3 makes `wouldChange(Reset)` true while a reading is in flight. This is the fix I asked for, plus the gap cap.            |
| M2 cap, interleave and survival conflict | Resolved | R3-7. 5.3 chooses strict priority, counts every turn of a change before it starts, applies the cap to source changes, deletes the interleave sentence and its test, and prices the wake (`controlPerWake + 4P` turns, 4.4e8 instructions). The reset survival test is now consistent (5.4).               |
| M3 charge identity under close-on-accept | Resolved | R3-4. 3.2: a closed charge moves into its note (`PostBox::Item::charge`); the map holds the open charge only; bound 1 + `ingress_queue_per_source` = 9; `abandon`, `dropHostEvents` and a superseded note fold `content` and `other` into the lifetime counters. No identity key is needed.               |
| M4 replay past the outcome log           | Resolved | R3-5. 3.2: a host high-water mark per stream raised only by a commit or a pass credit, plus a bounded ring above it. The residue (a relay recounts the newest-ring-plus-one distinct refusals until the device next commits; marks are lost on restart) is stated and is an owner question (14).          |
| m1 to m16                                | Resolved | Section 12 maps each to a section and I checked them against the text: 5.1 thread contract, 5.2 digest and lock order, 5.3 batch expansion and the cap on source changes, 6.3 `Clear {}` exception, 6.4 stream creation and retire, 6.8 `cellCount()` assertion. m10 and m15 are corrected in 4.1 and 13. |

## What is sound

- The settle loop contract in 4.1: one top-level event per turn, inline, with the discard in
  executor C++ so recovery does not depend on the VM budget that failed. Termination is argued
  correctly. One publication after the loop, and `SettlePending` for a checkpoint.
- The mark in 3.2 separates "counted once" from the evictable outcome log, and the decision travels
  with the queued item, so a late credit cannot reclassify an earlier queued reading.
- The reset generation scheme in 5.4: stamped with the push under one critical section, set at
  commit, checked again in `deliverHost`. With strict priority a reading queued after the call has a
  generation equal to `requested` and is not superseded.
- R3-13 is the right shape for the flood the hardware reviewer measured: a negative cache, never an
  authority, with one test per transition.

## BLOCKING

None.

## MAJOR

None.

## MINOR

- m1. The latch cache (6.5) is set and cleared "in `finishHealth`", which 4.1 now runs after the
  settle loop. A reset commits in the arena first, then up to `4P` settle turns run, each under a
  full instruction grant. For that interval `observe` still refuses `SourceFaulted` on another
  thread for readings the arena would accept. The device sees a terminal `NeedsHostAction`, not a
  retryable backpressure. Set and clear the cache when the change turn commits, before the loop.
- m2. 6.5 says the audit sink runs "after a health turn finishes and before its outcome is visible",
  and 4.1 publishes once after the settle loop. Fix the order in both places: turn, settle loop,
  sink, publication. Then the sink sees the `SettleReport` and a settled state (it may call
  `sourceHealth`), and `healthOutcome` shows `Pending` until the sink returns.
- m3. 4.1 says nothing can leave `d.pending` linked between wakes except a crash. `checkpoint` and
  `upgradeRefusal` read `hostEvents`, and `settle()` swaps `hostEvents` into a local batch, so a
  host callback invoked from `drain` during a wake (observer, host sink, audit sink) sees an empty
  `hostEvents`. `upgradeRefusal` refuses on `old.inTurn`; `checkpoint` does not. State that
  `checkpoint` refuses while `state.inTurn` and add that to the 2b test list.
- m4. 4.1 does not say what the loop and the publication do when the actor stops or an exception
  leaves the loop. Require publication on every exit (a scope guard), the report so far, and say
  that leftover events stay linked, so `checkpoint` refuses until a later change or an ingestion
  turn settles them (an ingestion turn runs `rtSettle_` over the whole queue, so a stale
  hard-failing event would fail every reading; discard on entry is the alternative).
- m5. 5.4 says a `NoChange` reset leaves `requested` unincremented. The call-time `wouldChange`
  decides that; a turn-time `NoChange` after a state change in between leaves `requested` ahead of
  `committed`. This is harmless (items stamped at the new `requested` are not superseded by a lower
  `committed`), so say so.

## Questions for the owner

None new. The two open ones (14) stand: persist the estimator mark, and "no durable record, no
isolate".

## Verdict per slice

- 2a: PASS. 2b: PASS. 2c1: PASS. 2c2: PASS.

Tally: 0 BLOCKING, 0 MAJOR, 5 MINOR.

Verdict: PASS
