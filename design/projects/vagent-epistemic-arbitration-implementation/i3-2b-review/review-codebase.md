# I3-2 slice 2b implementation review, round 1: compiler and codebase expert (persona codebase)

Scope: the uncommitted change in `/tmp/p19-2b/wt` (base `1140853d`, patch
`/tmp/p19-lead/2b-r1.patch`, 10 files, +932/-106) and the worker handoff `/tmp/p19-2b/handoff.md`,
against section 4 of `i3-2-design.md` (revision 3 as built, revision 4 as current text). Code
baseline is the worktree; the main tree at `d710e7a3` does not contain the change. Read-only. I ran
the worktree's `vagent_actor_test` with `--gtest_filter='VagentEpistemicSettle*'` (18 passed) and
built three throwaway probes of the test file under `/tmp/p19-codebase/r4/` (copy of the test with
extra prints, linked against the worktree objects; nothing in the worktree or repository was
edited).

## What is sound

- **`rtSourceDrop_` against the old walks.** I compared the three blocks line by line
  (`compiler.cpp` old `emitSourceChange` advance branch against `emitRtSourceDrop`):
  - Walk 1 (retire records with `incarnation < incBelow`) and the trim are the old code with `inc`
    replaced by the parameter. Same clone-rank order, same pinned/`noView` handling.
  - Walk 2 differs by one added `status == retired` test. At the advance every record visited has
    `incarnation < inc` (the advance runs only for `inc` above the profile's), and `observe`'s
    routine always writes both fields, so walk 1 has retired every one; the test is a no-op there.
    With `incBelow = INT64_MAX` (2c2) walk 1 retires every record, so it is a no-op there too. See
    m4.
  - The event queue order is unchanged: `rtBDrop_` appends to `d.pending` in walk order
    (`emitAppendTail`), and `rtSettle_` read it from the head before and the settle entry reads it
    from the head now. What changes for programs is only the cascade order (depth-first inside the
    parent's turn, breadth-first before), which section 4.1 and section 11 already record. The
    handoff's "newest key first" is the clone-rank order at HEAD too, so it is not a new reordering.
  - Registers: the routine copies `a0..a2` into its own cells before calling `rtBDrop_`, which
    overwrites them. The caller re-loads `a0..a2` and ends `freshWindows`, `endObservation`
    unchanged. `rtSourceDrop_` is emitted under the same `arbitrated` guard as its only caller.
- **The settle entry.** `emitSettleEntry` reads `rest = next(h)` before breaking either link,
  re-links `h` alone, calls `rtSettle_` (which resets `roundsG_` and detaches per round), and
  re-attaches `rest` after the queue is empty. A failure undoes the whole turn, so the head is back
  at the front for the executor. Multi-event queues (12 and 16 events) exercise the `rest != 0`
  path.
- **`settleQueued`.** The bound is the queue length counted before the loop; each iteration removes
  one top-level event, settled or discarded, so it ends. A failed turn is undone by
  `arena.release(m0)` (`runTurnOnce` step 7) before the unlink, so `first` is valid.
  `link(agent, pending, POS, next)` evicts the old head and its link to `next`, as the design says.
  The outcome is published once, after the loop, from `deliverHost`; a refused change never reaches
  the loop (`code == Committed` is tested), and a change that queued nothing yields no report.
- **Stop mid-loop.** `state.context` is re-checked after every turn. A handler's `terminate` only
  sets `kFieldTerminating`, which `settle()` acts on after `deliverHost` returns, so the rest of the
  queue still settles first; that is benign.
- **Checkpoint.** Every entry in `compiler.cpp` ends in `rtSettle_` (the `callLabel(rtSettle_)`
  sites), so no ordinary turn leaves `d.pending` linked, and the new `SettlePending` refusal cannot
  turn a working checkpoint into a refusal. The check sits after `HostDeliveryPending`, as designed.
- **Entry plumbing.** `settleEntry` is created with `sourceChangeEntry`, remapped in `program.cpp`,
  in the `sound` list and in `CompiledAgent`; `grep sourceChangeEntry` shows no other site that
  needs a twin. A program without the entry cannot have queued events, because the old entries
  settled in-turn.
- **Gates.** `arenaSlots + slotReserve()` applies to settle turns only; the top-of-`deliverHost`
  gate for the change itself is unchanged. `slotReserve()` is zero for `arenaSlots = 1`.

## Placement after E1, and style

- `class Gen` members are declared in `compiler_gen.hpp`; `emitSettleEntry` is defined in the new
  `epistemic_emit_health.cpp` (namespace `xanadu::vagent::runtime::codegen`, include style as the
  other TUs); `headOf` and `emitRtSourceDrop` stay in `compiler.cpp`, which is what 4.2 says ("edits
  `compiler.cpp` in place and moves no other body"). The Makefile finds the new file and test by
  glob. Good.
- `settleQueued` is in `agent_executor.cpp` (+89 lines), not in the design's
  `agent_executor_health.cpp`, and the constants are in `runtime_abi.hpp`, not in
  `epistemic_abi_health.hpp`. The first is forced: `State`, `TurnSpec` and `HostEvent` are private
  types defined in `agent_executor.cpp` (`agent_executor.hpp:499-520` only forward-declares them),
  so a second TU cannot define the member. The handoff does not say so; the design should (m5).
- I could not run `clang-format-19` (not installed). The installed clang-format 23 flags one
  continuation in the test (`vagent_epistemic_settle_test.cpp:382`, the `run` lambda) and none in
  the production files; the differences between majors are known (AGENTS.md), so run `make format`
  before commit.

## BLOCKING

None.

## MAJOR

### M1. `ABatchTooBigForOneTurnSettlesEventByEvent` would pass if the settle ran as one turn

Problem. The test is the only one for the design's F9 claim: a batch too big for the instruction
grant as one turn still settles, because each settle turn has its own grant (4.3 table, row 4). It
grants `kBatchGrant = 400,000` instructions, but the whole 12-event retire costs far less.

Evidence. Probe copies of the test (`/tmp/p19-codebase/r4/probe.cpp`, linked against the worktree
objects, `--gtest_filter='*ABatchTooBig*'`):

```text
PROBE retire12 cycles 42700          (13 turns, all 12 events settled)
kBatchGrant = 400000 (as shipped)    OK
kBatchGrant = 20000                  OK   (each turn needs about 3.3k; one turn would need about 42.7k)
kBatchGrant = 2000                   FAILED (the grant does bite; ingestion no longer fits)
```

A single-turn settle of the same 12 events needs about 42.7k cycles at most, which is a tenth of the
shipped grant. At HEAD (one turn) the test would pass too. The handoff's mutation "settle only the
head" fails this test for a different reason (events left unsettled), so it does not show the
property. The claimed guarantee is therefore untested.

Consequence. A regression that settles the whole queue in one turn again (the wedge-prone shape 2b
exists to remove) keeps every 2b test green except the failing-handler ones, and the grant isolation
per turn is not pinned.

Required fix. Set `kBatchGrant` to a value between the per-turn need and the batch need (20,000
works, measured above), and add a control inside the test that the batch could not fit one turn:
assert `ctx().vm.totalCycles()` delta over the retire is above the grant (the delta is 42,700 and
the grant 20,000), so a later change of handler cost cannot silently void it.

## MINOR

- **m1. The whole-queue fallback undercounts.** In `settleQueued`, when `link` refuses, the code
  drops the whole queue, sets `queued = 1` to end the loop, and counts one discard. 4.1 says
  `discarded` counts every event. Count the remaining queue (the loop already knows `queued`) and
  `break`, instead of the `queued = 1` trick. The fallback has no test (4.3 lists none, and a
  refused `link` on a ref read from the same arena should not occur); add one with a stale ref, or
  state it is unreachable.
- **m2. "Mints no cell" is not true of the discard.** The unlink mints none, but `bumpStat` calls
  `flushStats` when the actor is between turns (`agent_executor.cpp:1179-1181`), which writes the
  stat cells. At the hard cell limit that write can throw where 4.1 says the discard "cannot fail".
  The reserve covers it in practice; say so, or batch the stat flush after the loop.
- **m3. Price: slots.** 4.3 says "No arena cells". Each turn leaves slots (turn record, turn
  counter, stats) and slots are never reused: the probe measured 1,139 slots over the 17 turns of a
  16-event retire (`PROBE slots over retire16`), about 67 per turn including the retire's own work.
  Negligible against 2^30 but the price section should give the figure, since a change now costs
  `1 + n` turns.
- **m4. The walk-2 status test is redundant for every planned caller** (advance and
  `incBelow = INT64_MAX`), and costs a `find` and a compare per record. Drop it, or record in 4.2
  which caller needs it. If it stays for a future partial reset, a 2c2 test must cover a live record
  next to a retired one; none can now (handoff: "equivalent for the advance").
- **m5. Design text to align with the build** (handoff deviations 1 to 4, which I accept):
  1. A `fail` statement in a handler is contained by its own choice point (`emitHandlerRun`), so
     only a hard failure (a cascade past `belief_event_depth`, cycles, fuel) fails a settle turn or
     wedges an ingestion. 4.1 and the wedge test should say "hard failure", not "a handler that
     always fails". The tests use a runaway cascade, and one test uses a 10^8-iteration loop.
  1. `IngestOutcome::settle` is absent when nothing was queued. Say so in 4.1.
  1. "Before a mailbox turn" holds for later wakes only: in a wake that carries a message, the
     message turn runs first, exactly as at HEAD. Reword the 4.3 row.
  1. The discard test cannot use an instruction grant of 1 (it fails the change turn too); it uses a
     cycle-starved settle turn and a closed slot gate. Reword the row.
  1. Placement (see the placement section above): `settleQueued` stays in `agent_executor.cpp` until
     the private executor types move to a private header.
- **m6. Revision 4 items not built.** (a) 4.1's clamp of the reserve below `ephemeralBit` with a
  construction WARN is absent (`slotReserve()` is `arenaSlots / 64`, unclamped). With the default
  2^30 it cannot matter; with `arenaSlots` near 2^31 the arena throws mid-turn, caught as a failed
  turn, so the effect is a discarded batch, not a crash. (b) The revision 4 wedge test lists "a
  non-replacing reading of another key commits" and `stat()` counting `refused.IngestionFault`;
  `AnIngestionWedgeKeepsRefusingAndRetireStillCommits` checks neither.
- **m7. Test hygiene.** `ARetireOfSixteenEventsCostsOneTurnEach` prints `MEASURE` with `fprintf` and
  asserts no bound, while 4.3 gates "completes in one wake" and "delays the next ingestion by no
  more than the same 13 turns". Assert the turn count (done) and a cycle bound derived from the
  ingestion measure, or move the numbers into the design and use `RecordProperty` instead of a
  stderr print (AGENTS.md prefers named log categories to ad-hoc prints).
  `ATinyAllowanceKeepsWorking` only shows that ingestion is refused `ArenaExhausted`; no settle runs
  with `arenaSlots = 1`, so it does not exercise the zero reserve on the settle gate.
- **m8. Comment.** The block comment before `emitSourceChange` reflowed to a one-word line
  ("occurrences, and"); rewrap it.

## The four unmutated guards

- **`chargeable` exclusion of `SettleFailed`.** `Ingress` is private and no path charges
  `SettleFailed`, so the exclusion is a guard for a future caller. `ASettleNeverChargesTheSource`
  covers the observable effect (counters unchanged, no bookkeeping turn: `turnCount + 2`). Adequate.
- **`sound(settleEntry)`.** Needs an instantiate that returns a foreign cell; no test seam exists.
  The line is the same shape as its four neighbours. Adequate.
- **The loop bound.** Not reachable with real IR (a settle turn that commits without consuming its
  head). The ghost-event test shows the loop consuming a queued event it did not raise. Accept; note
  that a test double for `settleEntry` would be the only way to cover it.
- **The walk-2 status check.** See m4: equivalent for all current callers.

## Questions for the owner

None.

Tally: 0 BLOCKING, 1 MAJOR, 8 MINOR.

Verdict: FAIL (one test-power fix, M1; the production change itself has no open correctness
finding).
