# I3-2 design review, round 3: System architect (persona architect)

Scope: `i3-2-design.md` revision 3 (uncommitted, 1,136 lines), `decisions.md` section 1,
`findings-r2.md`, my own `review-architect-r2.md`, against P19 v1.5 sections 2.1, 2.2, 4, 5.1 and
8.2, the owner's decisions Q1 to Q3 and the I3-2 decisions, and `remaining-plan.md` section 4.2
(file growth, budget parity). Code at HEAD `8a4089d9` plus the uncommitted T0 diff in the working
tree (`git diff -- apps tests`). Scripts and output are under `/tmp/p19-architect/` (`r3.py`,
`r3b.py`, `r3b.out`, `r3sim.py`).

## Earlier findings

| Finding                         | Status                      | Why                                                                                                                                                                                                                                         |
| ------------------------------- | --------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1 departures not recorded      | Resolved                    | Section 11 lists A1-A10. A2 records the three settings, the second SUSPECT source and the resume rule (`R`, `N` read under the settings in force, clamped at the next update). A5 words `Clear` as operator-origin only.                    |
| M2 ungated `upgrade` overload   | Resolved                    | T0 adds `upgradeRefusal` (`agent_executor.cpp`, diff hunk at `@@ -5049`). Both overloads now check `inTurn`, `hostEvents`, the post box, choice depth and marks, and refuse populated `d.trust`/`d.epistemic`. See m7 for one missing test. |
| M3 exemptions not implementable | Partly resolved; see M1 now | The slot gate is named and given a reserve (4.1, 5.4). The cell reserve is now inside the ceiling (6.6), but it is charged to tombstones, which nothing bounds (new M1).                                                                    |
| M4 wedge at its edges           | Resolved                    | The discard is executor-side C++ with a whole-queue fallback (4.1). A failed event leaves the change `Committed` with a `SettleReport`. The ingestion residual is recorded as A4 by the owner's answer.                                     |
| M5 audit flush                  | Resolved                    | Eviction by class (6.5): repeats merge into `count`, and an ordinary entry never evicts an isolate or reset entry. The false "host log" sentence is deleted. The audit sink is the durable record.                                          |
| M6 settings                     | Resolved                    | `min_events` is capped at `floor(0.9/(1-b))` by `validate()` (3.3), the default is 8, and the count is corrected. I recomputed 7.43 at 8 events and 8.27 at 9. Whole-block fallback is the lead's F30(A) decision.                          |
| M7 health classification in C++ | Resolved by decision        | F6(a): one `health` count and a versioned table, justified in section 8. The relay asymmetry (m5) is gone because relayed refusals on signed fields now count in both `R` and `N`.                                                          |
| M8 host latch mirror            | Resolved                    | The mirror is dropped (F23, section 10).                                                                                                                                                                                                    |
| m1 citations                    | Resolved                    | Sections 2 and 3 cite `compiler.cpp:5517`, `epistemicCellsWithin` by name and the HEAD line numbers; I spot-checked 25 and all match. One residual in m8 below.                                                                             |
| m2 slice independence           | Resolved                    | Four slices, `sourceHealth` added to 2a, `rtSourceDrop_` moved to 2b, each with its own gate.                                                                                                                                               |
| m3 `CompiledAgent` growth       | Resolved                    | Remap, `sound` list and an ABI pin are required for both new entries (section 7), and section 13 adds an after-`instantiate` test. The per-actor IR footprint is still unstated; see m7.                                                    |
| m4 settle and discard entries   | Resolved                    | Discard is C++, settle is an entry (4.1).                                                                                                                                                                                                   |
| m5 relay asymmetry              | Resolved                    | 3.2 counts relayed outcomes once per event in both `R` and `N`.                                                                                                                                                                             |
| m6 reset bumps before commit    | Resolved                    | `ResetMark {requested, committed}` (5.4); supersession depends on a committed reset.                                                                                                                                                        |
| m7 `suspect` without producer   | Recorded                    | Kept, tested with a synthetic profile (3.4, 3.8).                                                                                                                                                                                           |
| m8 grant details                | Resolved, one residual      | Issuer trust stated (5.1), bind-time check added, 24 h bound, pending bounds are settings. `Ingress::everBound` is unbounded; see m5 below.                                                                                                 |
| m9 determinism of `b`           | Resolved                    | Computed in the IR through `Exp` from the integer `H` (3.2).                                                                                                                                                                                |
| m10 vocabulary                  | Resolved                    | `ResetMark` (host) versus `reset_epoch` (arena, stamped into `calibration_epoch`); `health_weight`.                                                                                                                                         |
| m11 `Clear` semantics           | Resolved                    | Absent code is `NoChange`; another origin refuses `NotOperatorReason` (6.2).                                                                                                                                                                |

Both round-2 owner questions are answered and recorded (A7, A4). The "Fit to later iterations" items
I raised (reason origin, arena-side epoch, discard count in `stat()`, the translator's carry list)
are all in section 13.

## What is sound

- **Slice order and the stub.** 2b before any health action, and 2c1 against a stub turn, means the
  authorization surface is reviewable before it can change state. 2c1 depends on 2b only through the
  slot reserve; 2c2 depends on 2a (reset zeroes `R`, `N`), 2b (the drop) and 2c1. Each slice's gate
  names its tests. 2a and 2b do not depend on each other.
- **The estimator figures hold.** I re-simulated the whole 3.3 table with the stated rule
  (`r3sim.py`): SUSPECT share and transitions per hour match to the printed precision (for example
  20% at H 32: 12.0% and 132/h; 30%: 85%). Clean history to SUSPECT is 14 events, recovery is 64,
  and the start-up false-alarm rates are 1.9% and 10.7% in the first 12 events.
- **Settle design.** The termination argument in 4.1 is correct (the head event is removed per turn
  and only a settle turn adds events). The discard is a link edit on a ref read from the same arena,
  so it needs no VM budget. Publishing the outcome once, after the loop, with the floor raised on
  `Committed`, closes my round-2 point on `finishSourceChange`.
- **Reset generation.** Superseding by a committed reset only, with generation stamps on items and
  charges, removes the early-bump problem and survives a failed or revoked reset.
- **C++ versus Vortex.** Section 8 now lists each C++ responsibility with its category. The two
  host-side views of arena state (`sourceHealth`, `wouldChange`) are tested for equality with the IR
  rather than treated as a second authority.
- **Audit by class** gives a privilege order that a lower privilege cannot reverse.
- **Citations.** The `compiler.cpp` and HEAD `agent_executor.cpp` lines I checked (5089, 5136, 5369,
  5375, 5517, 5668, 5684, 4691, 4756, 4808; 1873, 1936, 3305, 3399, 3770, 3859, 3911) match.

## BLOCKING

None.

## MAJOR

### M1. The per-profile health reserve is charged to tombstones, which nothing bounds

Problem. Section 6.6 charges `healthBound - health_cells` against ordinary admission "per existing
profile, tombstones included", and justifies `validate()` by the bound
`max_sources x (healthBound + kProfileBaseCells) <= cells`. That bound covers `max_sources`
profiles. The number of profiles is not `max_sources`:

- `emitProfileFor` counts only live profiles against `max_sources`; a tombstone is skipped
  (`compiler.cpp:4706-4716`).
- A retire of a principal with no profile allocates a bare tombstone with no count check
  (`compiler.cpp:4929-4941`), and source changes run without the admit hook
  (`agent_executor.cpp:1865-1872`, unchanged by the design). `bindSource` has no limit on distinct
  principals (`agent_executor.cpp:3423-3500`).
- A tombstone holds its floor for the life of the actor; a checkpoint and resume copies it.
- The design's own `profiles_cap` of `2 x max_sources` applies only to profile creation by an exempt
  `Isolate` (6.2, 6.6). It does not stop a retire.

Evidence (`r3b.py`, `r3b.out`, defaults H 32, 3 reasons, 4 actions, `healthBound` 142, base 44):

| State                              | Tombstones before ordinary admission fails, at HEAD cost 44 | At revision 3 cost 186 |
| ---------------------------------- | ----------------------------------------------------------- | ---------------------- |
| no live profile                    | 558                                                         | 132                    |
| 16 fully active principals (1,520) | 5                                                           | 1                      |
| 128 live profiles, no records      | 17                                                          | 4                      |

At 256 profiles (`2 x max_sources`) the reserve is 47,616 cells against `cells + cells/16` = 26,112,
so the invariant that "a profile can always reach its health bound" is false for the case the design
itself admits. Once the sum of reserves exceeds the ceiling, the ordinary admission test
(`cellsWithin(all) + sum(reserve) <= cells`) is false for every ingestion, bookkeeping turn and
degrade, and no operator action recovers it: a tombstone cannot be removed (it holds the floor) and
a checkpoint copies it.

Consequence. A fleet that rotates device keys and retires the old principal (the documented
decommission path, `agent_executor.cpp:3498`) stops all ingestion on the actor after a few tens to
about 130 retirements. The lead confirmed this reserve on the stated condition that `validate()`
bounds the worst case at `max_sources` profiles; it does not.

Required fix. Charge the reserve to live profiles only (at most `max_sources`, so the `validate()`
bound is true), and charge a tombstone its actual cells (including its `health_cells`). An exempt
`Isolate` or `Clear` on a tombstone draws on the headroom, as an isolate-created profile already
does. State the bound on tombstones explicitly: either add a count to retire (a tombstone past
`profiles_cap` refuses the retire with a typed code and the host decommissions by unbinding
instead), or record that tombstone count is bounded only by the cell ceiling, as at HEAD, and give
the figure. Add a 2c2 test that retires `2 x max_sources` principals, then ingests, degrades and
checkpoints. Update `validate()` and 6.6 to say which profiles the bound covers.

## MINOR

- **m1. Reset floors can exceed what the stream would ever accept.** 5.4 raises `admittedSeq` in
  `observe` for every reading that passes its checks, and a committed reset raises each stream's
  floor to it (6.4). The arena writes the stream (sequence, epoch, mapping) only at commit
  (`compiler.cpp:5415-5430`), after the sequence test and the epoch test. A reading with a higher
  sequence that the arena then refuses (for example `ClockEpochRegression`, which this design makes
  a health refusal) therefore raises `admittedSeq` but not the stream. After a reset, the repaired
  device is refused `StaleSequence` until it passes that sequence, and no operator action clears a
  floor (F22 option B is not built). Take the floor from the readings queued at the call (the max
  over the principal's post-box and `hostEvents` items), since committed ones are already in the
  stream; or keep `admittedSeq` and record the lockout and its only exit (a higher incarnation).
- **m2. `retired_by` is sized three ways and has no stated home.** 6.4 says "2 cells per retired
  record, at most 16 a profile", 6.6 prices "24 cells of `retired_by`", 6.8 says "at most 32". It is
  outside `healthBound`, so the reserve does not cover it. State whether it is a named field
  appended after the 24 positional fields of `kObservationOrder` (`runtime_abi.hpp:602`) or a
  position, count it once, and say which budget pays for it.
- **m3. The 2b price ignores handler cost, and the per-wake cap ignores settle turns.** 4.3 bounds a
  change at "13 turns, below 56k cycles at the 4.3k an ingestion turn costs". A settle turn runs the
  program's `-pred` handlers, each with its own grant (`vagent.max_instructions`, default
  10,000,000, `system_docs.cpp:124`); before 2b all of them shared one turn's grant.
  `controlPerWake` (5.3) counts health turns, and `settle()` counts only ingestions
  (`agent_executor.cpp:1985-2008`). One wake can run 16 changes x 14 turns. Count settle turns in a
  per-wake turn budget, or state the wake worst case in instructions and have the gate measure it.
- **m4. Thread contracts for the new API are not stated.** `bindSource` says "call it from the
  thread that pumps" (`agent_executor.hpp:350`); `observe` says "any thread". 3.5 puts
  `sourceHealth` on the pump thread, but `Reset` needs `expectResetEpoch`, which the caller reads
  from `SourceHealth` (6.7). Say which of `grantHealth`, `revokeHealthGrant`, `healthAction`,
  `healthActionBatch`, `healthOutcome` and `sourceHealth` are any-thread, and how an operator thread
  obtains the epoch. State that the audit sink is called outside `ingress_->mutex` and that
  re-entering the executor from it is undefined or supported.
- **m5. `Ingress::everBound` is a host-wide set with no bound.** It grows with every principal ever
  bound on any actor, and `bindSource` has no limit on principals. Bound it, or replace it by a
  check against the grant issuers at bind time plus the profile check, which already covers
  everything except a principal bound earlier on another actor and since forgotten.
- **m6. `wouldChange(Reset)` is not defined.** 6.3 says a reset of a "clean" profile with no current
  record and a matching epoch is `NoChange`. A committed reset also raises stream floors, clears
  liveness stamps, zeroes `R` and `N` and supersedes queued readings (6.4). List what "clean" means
  (`R = N = 0`, no stamps, floors not above the streams, no reasons) and say that a `NoChange` reset
  supersedes nothing queued, since `committed` does not move.
- **m7. Test and measurement gaps against the plan's rules.** (a) The plan's budget-parity rule
  requires `AFullEpistemicBudgetStillCheckpoints` to be re-measured by a slice that adds cells. 2a
  adds Double scalar cells (`R`, `N`, `health_fraction`), which carry a text half and a bits half
  (`scalar.hpp`), so the "about two operations per cell" figure behind `epistemic_limits.hpp:36-41`
  may not hold; 2a's table does not list this re-measure. (b) 2c2 has no test that a latched profile
  survives both `upgrade` overloads (the refusal itself is T0's; the latch cannot be set before
  2c2). (c) Per-actor IR footprint: `settleEntry`, `healthEntry`, `rtHealth_` and `rtSourceDrop_`
  are compiled into every actor's arena; give the cell count before and after each slice, since they
  count against the checkpoint's operation limit. (d) The existing I3-1 budget tests that fill the
  ceiling with 18 principals change to 16 in 2c2; list them.
- **m8. Citation and format notes.** `kIngestOrder` is a local constant in `compiler.cpp:5264` with
  four entries, not an ABI constant in `runtime_abi.hpp`; 3.8 lists it under that file. Section 7
  says `kProgramFormat` "does not change again" after 2a, while 2c1 adds `$turn.health` and 2c2 adds
  `retired_by` and a new source for `calibration_epoch`. A bump changes every `programRef`
  (`program.cpp:19-23`), so say what rule decides when to bump and why none is needed after 2a.
- **m9. Settings domains are not independent.** `epistemic_health_reasons` in [2, 16] and
  `epistemic_health_actions` in [2, 64] give `healthBound` up to 1,550 (`r3b.out`); times 128
  sources is 204,032, which `validate()` rejects against `cells`, and the whole block falls back to
  defaults. The table in section 7 should state the joint rule, and the Schema text should say that
  raising either can invalidate `max_sources`.
- **m10. The once-per-event guarantee is volatile.** 3.2 says only a log eviction lets an event
  count twice. The log is a per-source ring of `outcome_log` (64) entries held in host memory and
  not checkpointed, so a restart or resume also empties it. A replayer holding more than 64 distinct
  captured bad events (signed-field refusals count for a relayed copy too) can recount them in
  rotation. Whether the staleness and incarnation checks run before the health checks decides
  whether this is reachable; the security reviewer should confirm, and 3.2 should state the bound
  that applies.
- **m11. Capacity at the defaults falls below `max_sources`.** A source with one record costs 44
  - 82 cells plus its 142 reserve: 268, so 91 sources fit in 24,576 cells (`r3b.out`). At HEAD the
    figure is 195, so the default `max_sources` of 128 was reachable. Section 6.6 reports only the
    fully active case. State the one-record figure in the Schema notes and in the price table, and
    see the question below.

## Questions for the owner

1. The reserve makes the default `max_sources` of 128 unreachable when sources are lightly active
   (91 at one record each). Should the default `cells` rise (the checkpoint limit is the binding
   constraint, `epistemic_limits.hpp:36-41`), should `max_sources` drop to match, or is the reserve
   to be charged lazily, only to profiles whose health state has grown? The answer changes M1's fix
   only in detail.

Tally: 0 BLOCKING, 1 MAJOR, 11 MINOR.

Per slice:

- 2a: PASS
- 2b: PASS
- 2c1: PASS
- 2c2: FAIL (M1)

Verdict: FAIL
