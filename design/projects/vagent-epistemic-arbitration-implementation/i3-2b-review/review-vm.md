# I3-2b implementation review, round 1: Vortex VM and runtime expert (persona vm)

Scope: the uncommitted slice 2b change in `/tmp/p19-2b/wt` (patch `/tmp/p19-lead/2b-r1.patch`, based
on `1140853d`) and the handoff `/tmp/p19-2b/handoff.md`, against section 4 of the I3-2 design
(revision 3 in the worktree, which the worker implemented; revision 4 in `/data/git/gleditor`, the
current text). Code at HEAD `3b86b36d` plus the patch. Focus: the emitted IR of `settleEntry`,
`rtSourceDrop_`, arena marks and undo on a failed turn, the C++ discard, cycle and fuel accounting,
the depth bound, the slot gate and reserve, and every path that could leave `d.pending` in a state
the next ordinary `rtSettle_` would misread. Paths below are relative to `apps/common/xanadu/`
unless they start with `tests/`.

I ran `build/vagent_actor_test --gtest_filter='VagentEpistemicSettle*'` in `/tmp/p19-2b/wt`
(headless, isolated XDG under `/tmp/p19-vm/xdg`): 18 passed. I did not run `make` and did not mutate
code, so the mutation table in the handoff is taken as reported.

## What is sound

**`settleEntry` IR** (`vagent/runtime/epistemic_emit_health.cpp:19-38`). Traced instruction by
instruction against `VortexCore::breakLink` (`vortex/vortex_core.cpp:161-176,248`, which is
`arena.link(cell, dim, dir, nullopt)`) and `ArenaManifold::link`
(`zigzag/arena_manifold.cpp:1353- 1405`, which clears both ends and evicts displaced occupants):

```text
h    = agent.pending                ; 0 -> Committed, nothing to do
rest = h.pending
break(agent, pending)               ; agent<->h cut, both ends
if rest != 0: break(h, pending)     ; h<->rest cut, both ends
link(agent, pending, h)             ; queue is exactly [h]
rtSettle_                           ; drains h and everything h raises, queue ends empty
if rest != 0: link(agent, pending, rest)   ; queue is [rest...], rest.neg = agent
Committed
```

The remainder keeps its internal chain because only the two edges at `h` are cut. `rtSettle_`
(`vagent/runtime/compiler.cpp:4543-4590`) loops until `agent.pending` is empty, so the final `link`
never evicts anything. `h` and `rest` are plain `reg()` cells (`frame_ = nullptr`, so not frame
temps), unique to this routine, and the routine is not re-entrant. The `h == 0` and `rest == 0`
exits share one label correctly.

**Undo.** A failure anywhere in the turn (cycles, fuel exception, `failNow()` in `rtSettle_` at the
depth bound, a contract violation with no handler choice point) lands on `failOp`, and `runTurnOnce`
does `arena.release(m0)` (`vagent/runtime/agent_executor.cpp:2319`). The queue links of `agent` and
`h` are older than `m0`, so conditional trailing records every write and `release` restores
`agent -> h -> rest` exactly. Cells minted in the turn (cascade events, handler scratch) are
truncated away. Fuel is deliberately not part of `Mark` (`arena_manifold.hpp:170-182`, "failed work
costs fuel"), which is right: the next turn calls `setExecutionLimits` afresh
(`agent_executor.cpp:2176-2184`) so each settle turn has a full grant and none inherits another's
shortfall.

**C++ discard** (`agent_executor.cpp:1989-2010`). After a failed turn no mark is outstanding and
`liftLimits` has run (`:2333`), so the write is unlimited. `arena.link(agent, pending, POS, next)`
with `next = linked(first, pending)` evaluates, in `ArenaManifold::link`: `displacedUs = first`, so
`first.neg` is cleared; for `next` present `displacedIt = first`, so `first.pos` is cleared; then
`next.neg = agent` and `agent.pos = next`. With `next` absent only the first and last statements
apply. No branch mints a cell or calls `consumeWork` (`consumeWork` is reached only from `intern`
and `mintSlot`, `arena_manifold.cpp:1176,1189`), so the discard really is independent of the VM
budget. The only refusals are `UnknownCell/Dimension/Target`, impossible for refs just read from the
same arena.

**Termination and ordering.** Every iteration removes exactly one top-level event: a committed turn
consumes `h` and re-attaches the unchanged remainder; a failed turn is rolled back and the discard
removes `h`. A successful turn cannot leave extra events (`rtSettle_` drains to empty before the
re-attach) and a failed one cannot add any. The loop therefore never grows the queue, and its
`queued` bound is a backstop. The loop runs inline in `settle()` (`:2063-2085`) directly after the
change's `runTurn` in `deliverHost`; no VM code, host event or mailbox turn runs between the change
turn's commit and the first settle turn. `execute()` holds `state.inTurn` for the whole wake
(`:1464-1466`), and `upgrade` refuses on `old.inTurn` (`:5150`), so no checkpoint or upgrade path
from another caller interleaves with the loop in the single-threaded pump (threading of
`checkpoint()` itself, which does not test `inTurn`, was not checked).

**`rtSourceDrop_`** (`compiler.cpp:3668-3787`). Arguments are copied from `a0_..a2_` into private
registers before any call, and `a0_..a2_` are reloaded for `rtFind_` and `rtBDrop_` from those
copies. `incBelow` and `retained` arrive as cell refs and are read with `D(...)`, as the inline code
did with `D(inc)` and `D(retained)`. The first walk, the retired-chain move, the `rtBDrop_` call and
the trim are the old instruction sequence unchanged except for one added status test. The routine
and its call are emitted under the same condition (`any arbitrated`) as `observeEntry`, so no
dangling label. The `bad` exit after `ret()` ends the turn with `IngestionFault`; a halt from inside
a `Call` leaves the VM stack linked, which step 8 of `runTurnOnce` (`:2339-2343`) unlinks. The
events `rtBDrop_` queues carry a copy of the term (`emitRtBRaise`, `compiler.cpp:3293-3297`), so
settling them after the retire has excised the clauses and records reads nothing that is gone.

**Depth bound.** `roundsG_` is zeroed at the top of `rtSettle_`, so `belief_event_depth` now bounds
each top-level event's cascade independently, with the same off-by-one as before (the head is round
1). The runaway test exercises it end to end and the parent's turn alone fails.

**Failure containment of handlers.** `emitHandlerRun` (`compiler.cpp:5472-5493`) wraps every handler
in `Choice`/`Cut`, so a `fail` statement or contract violation backtracks to the handler's own
choice point. The worker is right that deviation 1 is a real defect of the design text: only a hard
failure (cycles, fuel, depth) fails a turn. Section 4.1 and the test table should say "hard
failure".

**Numbers.** The handoff's per-turn cost is (14,408 - 1,819) / 15 = 839.3 cycles, consistent with
the printed `MEASURE` line (I re-ran it:
`retire16: 14408 cycles over 17 turns; retire1: 1819 ... over 2 turns; one ingestion 4393`).

## BLOCKING

None.

## MAJOR

### M1. The slot reserve is not clamped below the arena's ref space and construction logs nothing

Problem. Revision 4 section 4.1 (current text, lines 610-617 of `i3-2-design.md`) requires that
"construction logs a WARN when `arenaSlots + slotReserve` reaches the arena's allocation limit,
`ephemeralBit` = 2^31 ... and clamps the reserve to the room below it, so a large setting cannot
push a turn past the limit where the arena throws mid-turn". `EpistemicLimits::slotReserve()`
(`vagent/runtime/epistemic_limits.hpp:108-111`) is `arenaSlots / 64` with no clamp, and nothing
warns. The worker implemented revision 3, which did not have this; revision 4 added it under R3-2.

Evidence. `arenaSlots` has no upper bound: `loadPositive` (`agent_executor.cpp:5737-5762`) accepts
any positive Int64, and `validate()` only rejects zero. The only ceiling inside a turn is
`allocationLimit_ = min(m0.cellCount + grant.maxCells, UINT32_MAX)` (`:2176-2178`), which can sit
far above `ephemeralBit`. `ArenaManifold::refOf(dense) = ephemeralBit | dense`
(`arena_manifold.hpp:498-501`), so a slot with `dense >= 2^31` has the same ref as slot
`dense - 2^31`: silent aliasing of two cells, not an exception. The settle gate admits a turn at
`cellCount < arenaSlots + arenaSlots/64` (`agent_executor.cpp:1979-1981`), so the reserve moves the
point at which a turn can start and run into the alias zone down by `arenaSlots/64` compared with an
ordinary entry. At the default (2^30 slots, reserve 16,777,216) a settle turn may start at 2^30 +
2^24 and has 1,056,964,608 slots of room below 2^31, so the defaults are safe (computed by
`python3 -c "print(2**31-(2**30+2**24))"`).

Consequence. An operator who raises `vagent.epistemic_arena_slots` toward the ref space (the
setting's own note says nothing about a limit) gets a reserve that lets settle turns mint past
`ephemeralBit`. The failure is memory corruption of the arena's identity space, in the one place the
design said it had made impossible.

Required fix. In `slotReserve()` (or where the limits are built from settings), return
`min(arenaSlots / 64, ephemeralBit - arenaSlots)` as a size_t with underflow guarded (zero when
`arenaSlots >= ephemeralBit`), log a WARN once at construction when the clamp bites, and add a unit
test with `arenaSlots = 2^31 - 1000` (reserve 1000 at most) and `arenaSlots >= 2^31` (reserve 0). If
the owner prefers to defer, the deviation must be recorded in section 11 with this scenario; it
should not be left implicit.

## MINOR

**m1. The whole-queue fallback miscounts and its own failure is silent**
(`agent_executor.cpp: 1993-2001`). When `link(agent, pending, POS, next)` is refused the code
unlinks the whole queue, sets `queued = 1` and counts one discarded event, though the queue held
`k >= 1` events; the report and `refused.SettleFailed` then understate the loss. The second `link`
is `(void)`-cast, so if it is also refused (same unreachable cause) `d.pending` stays linked and the
next ingestion's `rtSettle_` settles the poisoned head together with its own events, which is the
wedge this slice removes. Count the events in the dropped chain (the `queued` walk already computed
the length) and, on a second refusal, log at ERROR and stop the actor (or at least leave the
existing `SettlePending` checkpoint refusal as the only exit). Unreachable by construction, so
MINOR.

**m2. The `queued` count walk is unbounded** (`:1970-1971`).
`for (at = head(); at; at = linked(*at, pending)) ++queued;` loops forever on a cyclic chain. No
code builds one, but the loop exists to survive a defect; bound it by `arena.cellCount()`, as
`rankContains` bounds itself with `traversalBound()`.

**m3. `settled` overstates what ran.** A settle turn commits when every handler of the event either
succeeded or failed contained (the choice point swallows `fail`). `SettleReport.settled` counts it
as settled and the failed handler's effects are undone without a trace in the report or `stat()`.
That is the HEAD behaviour for contained failures and not a regression, but section 4.1 and the
`SettleReport` comment should say "turns that committed", not "events handled", so an operator does
not read `discarded == 0` as "every handler ran to the end".

**m4. Deviation 3 is stated more weakly than the code is.** The handoff says the loop runs "before
any other host event or later wake" and that "before a mailbox turn" holds only for later wakes. The
code gives the stronger guarantee that matters: after the change turn commits, nothing (host event,
mailbox turn, dispatch, intention) runs for that actor before its settle loop ends. A message turn
in the same wake runs before the change turn (as `execute()` has always ordered it), so it cannot
see the committed change with events still queued. Reword the design line to "before any turn that
follows the change". The `!state.context` checks in `settleQueued` are unreachable from inside a
wake (`state.context` is cleared only by `orderlyStop`, called from `settle()` after the loop);
harmless defensive code, but the comment should not imply a stop can happen mid-loop. A `stop`
statement in a settle handler sets `$terminating`; the loop keeps settling the remaining events and
`settle()` stops the actor afterwards, as it does for later host events.

**m5. Cost amplification is per change, not per wake.** `ingestPerWake` (default 4) counts only
ingestions (`agent_executor.cpp:2070`). Each source change now costs `1 + n` turns,
`n <= keys x predicates`, and a failing settle turn burns a full instruction grant before it is
discarded. A batch of `K` changes in one wake costs `K (1 + n)` turns with no yield, where HEAD cost
`K`. The measured 839 cycles a benign settle turn is fine, but a handler that always exhausts cycles
turns every change into `n` full grants (`n` is 4 per predicate at the defaults). Source changes
need an authenticated binding, so this is not an open attack surface; record the multiplier next to
the price in 4.3.

**m6. Stats from a failed settle turn are counted three times.** `runTurnOnce` bumps `dropped` and
`failed.<cause>` for the undone turn (`:2349-2351`) and `settleQueued` bumps `refused.SettleFailed`.
Both are correct per the design, but section 4.1 names only the last; document that `dropped` also
rises so operators do not double count.

**m7. The drop order is not a change from HEAD; the test comment and handoff item 6 say it is.** The
walk over the profile's `d.clone` rank is the same instruction sequence in the old inline code and
in `rtSourceDrop_`, and `rtBDrop_` queues in walk order, so the top-level queue order is unchanged.
What changed is how a cascade interleaves (2717 instead of 2177 in the test). Two program-visible
facts to state in section 11: top-level order is "newest record first" as before, which programs
could not rely on across keys at HEAD either; and the cascade is breadth-first inside its parent's
turn (`rtSettle_` still detaches a round at a time) and depth-first only across top-level events, so
"a cascade runs depth-first" in 4.1 should read "a cascade completes before the next top-level
event". I judge this safe for programs: the only order that changed is between a cascade and the
next top-level event, and a handler could not observe it at HEAD except through that interleave.

**m8. Two of the four unmutated guards are behaviourally dead and one is untested.**

- The `rtSourceDrop_` walk-2 status test (`compiler.cpp:3722-3728`) is equivalent to the old
  unconditional walk for the advance, and for isolate/reset with `incBelow = INT64_MAX` every record
  that has an incarnation field is retired by walk 1, so it differs only for a record with no
  incarnation field. `emitObserve` writes one on every record (`compiler.cpp:3603,4092,4260`), so no
  caller reaches the difference. Either remove it or add a test that builds such a record by hand;
  as written it is a guard that no test can observe.
- The `chargeable` exclusion of `SettleFailed` (`agent_executor.cpp:920`) can never fire because
  `Ingress` is private and the label never reaches `finishIngest`. `ASettleNeverChargesTheSource`
  checks the counters, which is the right observable; keep the exclusion as a tripwire and say so in
  its comment.
- The `sound(settleEntry)` guard (`:1293`) has no test. The existing tests for the other entries'
  `sound` checks, if any, were not checked by me; a test that hands `prepare` a program whose
  `settleEntry` names a missing cell would close it.
- The loop bound is a backstop for a defect and cannot be hit, as the termination argument above
  shows. The `queued` bound is adequately covered by the argument and by every test that ends with
  `queueEmpty()`.

**m9. Not checked.** (a) The commit branch of `runTurnOnce` calls `arena.intern("")` outside the
`try` (`:2329-2330`), which consumes work and throws if the work budget is exactly spent; this is
HEAD behaviour for every turn, but a settle loop would then abandon the remaining events and skip
`finishSourceChange`. (b) Whether a checkpoint written before 2b resumes into a program image that
has `settleEntry`; `AnUpgradedProgramKeepsItsSettleEntry` covers the upgrade route only. (c) The
compile-time cost of `rtSourceDrop_` (a few hundred cells per program that arbitrates a predicate);
no measurement was asked for.

## Questions for the owner

1. Settle events the gate refuses are discarded, not deferred, so a program that hits the allowance
   in the middle of a retire loses the `-pred` for every remaining event and its beliefs stay out of
   step with the arbitrated state until it re-queries. The design accepts this (A10). Should the
   report carry a distinct count for "gate closed" so an operator can tell it from a handler
   failure? Today both land in `discarded` and `refused.SettleFailed`.
1. `settle.names` are the first four distinct discarded types in discard order, which is newest
   record first. Is that the order the operator expects to read?

Tally: 0 BLOCKING, 1 MAJOR, 9 MINOR.

Verdict: FAIL
