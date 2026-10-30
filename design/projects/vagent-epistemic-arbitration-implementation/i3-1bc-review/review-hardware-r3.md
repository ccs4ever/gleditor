# Hardware and fault-tolerance review, round 3: I3-1b / I3-1c / I3-1d

Scope: head `08425c60` (`git diff d48e85f8..08425c60`). Read-only. Probes in
`/tmp/i3-1bc-hardware/probe2.cpp` and `probe3.cpp`, relinked against the objects in `build/` (current
at this head), headless. `build/vagent_actor_test --gtest_filter='VagentEpistemic*'`: 97 tests pass.

## Probe results

| probe | result |
| --- | --- |
| retire(5), then bind(7), no pump between (round-2 N1) | last change `Committed`; an inc-7 event ends `Committed`. Fixed |
| same with a pump between | `Committed` |
| 40 boots, 4 fresh keys each, pump per boot | epistemic cells 117, 188, 614 (boot 8), 1182 (16), then 1253 flat through boot 40; every advance `Committed` |
| bind on a bogus pid, 20000 times | all `UnknownActor`, RSS +0 (was +26 MB) |
| 16 sources fill 64 slots, `pump(1)` | 4 free, 4 committed, 60 pending (unchanged, correct) |
| 100 sources, one event each | 64 accepted (recorded in section 14, not changed) |
| neighbour and unbound floods vs a committed outcome | outcome kept |
| actor stopped with 5 queued events | `ingestOutcome` nullopt for all five (see MINOR 1) |

## Round-2 findings

- MAJOR 1 (source-change order): fixed. `PostBox::pushControl` (agent_executor.cpp:590) inserts
  after the leading run of source changes, under the post-box mutex, and `bindSource`,
  `retireSource` use it. Kept items re-enter by `putBack` in their original order, so control
  items stay first. Probe N1 passes; a unit test is in the diff.
- MINOR 1 (queue counters never erased): fixed. `release` erases at zero, and `observe` tests with
  `find`, so a refused observe makes no entry.
- MINOR 2 (queue oversubscribed by default): recorded in section 14, still true (100 sources -> 64
  slots). Acceptable as recorded.
- MINOR 3 (stop paths disagree about outcomes): remains. See MINOR 1 below.
- MINOR 4 (`bindSource` accepts any pid): fixed (`UnknownActor`, probe above).
- MINOR 5 (`2 x max_age`, typed resume refusal): the age is recorded; the changed-policy resume
  still throws `std::runtime_error` (agent_executor.cpp restoreState, caught in `prepare`). The
  section 14 note that every exception in a turn is classed as a fuel failure covers the turn path
  only, not this one. Still open, cosmetic.
- MINOR 6 (orphaned doc comment): fixed, `percept`'s comment moved back to `percept`.

## BLOCKING

None.

## MAJOR

None.

## New code: the advance drop (emitSourceChange, compiler.cpp ~4680-4750)

Checked and sound:

- Termination and bounds. The drop walks the profile's clone rank once; a record already moved has
  the profile as its head on `d.epistemic`, finds no `d.source` link and is skipped, so a second
  advance never moves a record twice. The tally is O(n), the trim restarts from the oldest and
  stops at the end of the chain when everything left is pinned, so it cannot loop. The boot-loop
  probe plateaus at 1253 cells over 40 boots with 160 distinct keys; before the change the design
  records the cell ceiling reached at about 86 boots.
- Trim removes the record's clone occurrence with it (`unlinkFromRank` on both ranks), and
  `epistemicCellsWithin` now admits the profile's retired chain, so the ceiling counts what the
  routine keeps. `restoreState` copies `d.epistemic` for profiles, so resume carries the chain.
- Fuel. Worst case is a source across many predicates: records moved are bounded by predicates x
  keys x history, the trim is quadratic only in the pinned-prefix case, and an exhausted turn is
  undone and reported `TurnExhausted`; the host retries by rebinding. No unbounded loop.
- `finishSourceChange` on `EpistemicBudget` revokes the host binding without raising the floor, so
  the tombstone the arena keeps cannot silently take readings and a later rebind at the same boot
  works once room exists. Correct.
- Retire floor: with no live binding and no known floor nothing is created; the replaced default
  `0` floor was a sentinel and is gone. `held` is built from `optional::value_or` on a value
  that is immediately max-ed, not a stored absence.
- Observe: the field-order self-check and `readAt` refusing a short record are cheap and fail
  closed. The refusal path no longer overwrites a `Pending` entry, and a resend of a held or
  queued event is not charged twice. Lock order is unchanged (ingress mutex, then post box).

## MINOR

1. **Outcomes of a stopping actor.** `orderlyStop` clears `hostEvents` and calls `forgetIngress`,
   so the five queued events of the probe have no outcome afterwards, while `fatal` and a failed
   wake log `Undelivered` into a map that is then never freed (a leak of at most one log per
   source). Section 14 says an ingestion that can never run is logged `Undelivered`. Choose one
   behaviour: log `Undelivered` for the cleared events and drop the whole log with the actor, or
   keep nothing and delete the sentence. Not worth a round.
2. **Typed resume refusal for a changed policy** (see round-2 MINOR 5): throw a named exception so
   a host can tell a policy change from a corrupt checkpoint.
3. **`epistemicCellsWithin` justification** is now a comment (host admission, between halt and
   commit). Adequate for the Vortex rule.

Sentinels: none found in the diff. Vortex rule: the new advance drop, trim and settle are emitted
IR run by the VM; the new C++ is ingress, host admission and clocks.

Verdict: PASS
