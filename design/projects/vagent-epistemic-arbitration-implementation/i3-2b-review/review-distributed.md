# I3-2 slice 2b implementation review, round 1: distributed-systems expert (persona distributed)

Scope: the uncommitted change in `/tmp/p19-2b/wt` (based on `1140853d`): `agent_executor.cpp/.hpp`,
`compiler.cpp`, `compiler_gen.hpp`, `epistemic_emit_health.cpp`, `epistemic_ingress.hpp`,
`epistemic_limits.hpp`, `program.cpp`, `runtime_abi.hpp`,
`tests/xuzz/vagent_epistemic_settle_test.cpp`, and the handoff `/tmp/p19-2b/handoff.md`. Judged
against section 4 of the I3-2 design (revision 3 as implemented, revision 4 as current), with the
focus on turn ordering, the atomicity of the change-then-settle sequence, the one publication, the
delivery of discarded events, and checkpoint or upgrade during the loop. Line numbers are the
worktree's. I ran `build/vagent_actor_test --gtest_filter='VagentEpistemicSettle*'` in the worktree:
18 passed. I did not rebuild; the finding M1 is from reading the code and is not run.

## What is sound

- Turn ordering. `deliverHost` runs `settleQueued` inline after the change turn and before
  `finishSourceChange` and before returning to the batch (`agent_executor.cpp:1900-1911`), so no
  later item in the batch and no later wake sees the change without its settle. Control items
  already sort ahead of observations (`pushControl`), and
  `TheSettleLoopRunsBeforeAQueuedObservation` distinguishes the order (trace 212; a deferred loop
  would give 221).
- `settleEntry` (`epistemic_emit_health.cpp:21-37`) detaches the queue, puts the head alone, calls
  the unchanged `rtSettle_`, re-attaches the rest. A failure undoes the whole turn by
  `arena.release` (`runTurnOnce` step 7), so the queue is back as it was and the head can be
  unlinked in C++. The five-distinct-types test (`discarded == 5`, queue empty) is evidence that the
  remainder survives an undo. Handlers of different events cannot interleave: events raised by `h`
  land on a queue that holds only `h`.
- The discard (`agent_executor.cpp:1995-2004`) is a pure link edit with no VM call, and the loop
  bound `queued` is taken before the first turn, so the loop cannot run longer than the queue it
  started with. `AStarvedSettleTurnIsDiscardedWithoutTheVm` (a 10^8-iteration handler) and
  `TheDiscardSucceedsWithTheSlotGateClosed` cover the cycle and gate failure paths.
- Outbox order. Each settle turn drains its own outbox in `runTurnOnce` step 9 and a request ID
  comes from `$turn_count`, which every settle turn increments, so IDs stay unique and monotone. A
  failed turn leaves an empty outbox.
- Checkpoint and upgrade. `checkpoint` refuses `SettlePending` on a linked `d.pending`
  (`:5029-5034`). `upgradeRefusal` refuses on `old.inTurn` (`:5150`), and the whole loop runs inside
  `execute`, which sets `inTurn`, so an upgrade cannot run in the loop. The schema route also
  refuses a linked `d.pending` (`:5160-5163`). Between wakes the queue is empty by construction.
- The drop order. `rtSourceDrop_` walks the same clone rank and calls the same `rtBDrop_`, so the
  queue's order is unchanged; what changed is that a cascade runs in its parent's turn. That is a
  documented behaviour change (4.1, section 11), not an ordering bug, and no program can see a
  different order of top-level events. I accept deviations 1, 2, 3 (as HEAD behaviour: the message
  turn precedes a change that had not yet been applied), 4 and 6. Deviation 5 (walk 2 drops only
  records this call retired) is equivalent for the advance: every record of a profile has an
  incarnation below the new one, and walk 1 retires them all first.

## BLOCKING

None.

## MAJOR

### M1. A no-op rebind of another predicate erases the settle report

Problem. `finishSourceChange` stores the outcome with `insert_or_assign` on (actor, principal)
(`agent_executor.cpp:2013-2021`). `bindSource` queues a source-change turn for every bind, including
a rebind at the profile's current incarnation (`:3566-3571`: "At the profile's incarnation it
changes nothing"). A principal bound on two predicates is advanced by the first bind at the new
incarnation (queuing `-pred` events) and then "advanced" again, as a no-op, by the second. The
second change commits with no events, so its outcome has no `settle` (deviation 2), and it replaces
the first one.

Evidence. Reading `bindSource` and `finishSourceChange` above; the tests bind a single predicate
(`bindAll` binds `{"at"}`) and so do not meet it. I did not run it.

Consequence. For the normal case of a source bound on several predicates, the caller who reads
`sourceChangeOutcome` after the batch sees `Committed` with no report even though events were
discarded. The design says a caller never sees a committed change with a stale report, and the
report is the only per-change channel; `stat()` keeps the count but not the names or which change.

Required fix. Accumulate: when the stored outcome for the principal is `Committed` with a report and
a new `Committed` arrives, keep the old report and add the new one (sum the counts, append names up
to four distinct). Reset the accumulation when a change for a higher incarnation or a retire starts.
Test: bind a principal on two predicates, advance both with one failing handler, and assert
`discarded == 1` and the name after the second bind.

## MINOR

- m1. `settleQueued` breaks on `!state.context` after a turn before counting that turn as settled
  (`:1985`). Count a committed turn first. The same exit, or an exception from `arena.link` or the
  post-turn `expectWritten` calls, skips `finishSourceChange`, so the outcome stays `Pending`
  (before 2b it was published right after the one turn). Publish from a scope guard.
- m2. The fallback when `link` refuses drops the whole queue but adds 1 to `discarded` and names
  only the head. Count the events dropped (the number is known: `queued`). The branch has no test.
- m3. A stale queue (left by an exit as in m1) is settled by the next change, whose report then
  counts it (`ACheckpointRefusesLinkedPendingEvents` expects `settled == 2`), and by the next
  ingestion turn, whose `rtSettle_` runs the whole queue. A hard-failing stale event would then
  refuse every reading `IngestionFault`. Discard stale events on entry to the first change, or state
  the residual in 4.1.
- m4. `checkpoint` does not refuse `state.inTurn`. `settle()` moves `hostEvents` into a local batch,
  so a host callback invoked from `drain` (observer, host sink) during a wake finds `hostEvents`
  empty and, after the last settle turn but before `finishSourceChange`, a clear `d.pending`: a
  checkpoint then succeeds with the change committed and unpublished. Refuse on `inTurn` as
  `upgradeRefusal` does, and add a test that a checkpoint from a host sink in the loop is refused.
- m5. The slot reserve is not clamped below `ephemeralBit` and no WARN is logged, which 4.1 requires
  (`epistemic_limits.hpp:106-111`). With the default 2^30 there is room; `arenaSlots` has no upper
  bound in `validate()`, and a value above 2^31 minus the reserve makes the arena throw mid-turn,
  which `runTurnOnce` turns into a failed settle and a silent discard. Add the clamp, or list it as
  a deviation.
- m6. The interim state of 2b has no per-wake bound on control work: a wake with `k` source changes
  runs `k (1 + n)` turns. This equals HEAD's behaviour times `(1 + n)`; 2c1's `controlPerWake`
  (R3-7) closes it. Note it in the handoff so nobody reads 2b as bounded.
- m7. Unmutated guards. `chargeable` (`:920`) and the `sound` entry have no test that can fail, and
  the loop bound has none by construction. The `chargeable` exclusion is dead protection today
  because nothing routes `SettleFailed` to `finishIngest`; a one-line static test in a friend helper
  (or a `constexpr` predicate outside the private struct) would pin it. The `sound` entry can be
  pinned by instantiating against an arena that lacks the cell. The walk-2 status check is covered
  by 2c2's reset test and is fine to leave until then. 4.1 should say "hard failure" for the cycle
  or depth case, not "a handler that always fails" (deviation 1).

## Tally

0 BLOCKING, 1 MAJOR, 7 MINOR.

Verdict: FAIL (one MAJOR; the loop itself is correct)
