# I3-2 design review, round 3: sensor and hardware-fault expert (persona hardware)

Scope: `i3-2-design.md` revision 3 (uncommitted, 1,136 lines), my round-2 report
`review-hardware-r2.md`, `findings-r2.md`, and `decisions.md` (section 1 and the round-3 rulings
R3-1 to R3-11), against P19 sections 2.1, 5.1, 7 and 8.2 and the code at HEAD `3b86b36d` (T0 at
`4a912c69`; same code). The five finished round-3 reports (codebase, distributed, epistemology,
architect, security) were read for the findings that touch my discipline. Paths are relative to
`apps/common/xanadu/vagent/runtime/` unless they start with `design/`. Scripts are under
`/tmp/p19-hardware/r3b/` (`nums3.py`, `table.py`, `mark.py`, `mark_variant.py`, `poison.py`,
`flood.py`); every figure below was computed by one of them, each under `timeout 120`. My earlier r3
attempt (`/tmp/p19-hardware/r3/`) was not rerun; its `nums.py` result on subnormals is repeated by
`nums3.py` section d.

## Earlier findings

| Round-2 finding                                   | Status in revision 3                                                     |
| ------------------------------------------------- | ------------------------------------------------------------------------ |
| M1 relayed readings count in N, refusals do not   | Resolved (3.1, 3.2, decision F1(b)); replay hole is new M1 below         |
| M2 load bias: refusals counted, readings dropped  | Resolved in kind by the pass credit (3.2); its dedup is R3-5 / new M1    |
| M3 refusals classified by code name, not by cause | Resolved as the lead decided (F5(A)); the consequence is minor m1, m5    |
| M4 reflash runbook cannot work                    | Resolved: deleted, A3, 6.7                                               |
| M5 isolation is as durable as the checkpoint      | Resolved: 6.7 table, `checkpointed`, `NoChange`; the mirror part: M2     |
| M6 exempt actions unbounded                       | Resolved: `NoChange` without a turn (6.3), slot reserve, headroom        |
| M7 settle turn all-or-nothing                     | Resolved: one event per turn, discard alone, `SettleReport` (4.1)        |
| M8 operator cannot see the state                  | Resolved: `sourceHealth`/`sourceHealths` in 2a, tri-state outcome, batch |
| m1 minimum evidence                               | Resolved: `min_events` 8 as a weight (3.3); I confirm the numbers        |
| m2 slow source stays SUSPECT for days             | Recorded (3.3, Q15 answered "no forgiveness"); consequence correct       |
| m3 blanket clear removes sensor reasons           | Resolved: `origin` on every reason, `Clear {}` is operator-only (6.1)    |
| m4 refused Reset bumps the epoch                  | Resolved: generation, committed only on commit (5.4)                     |
| m5 audit order without a clock                    | Resolved: `seq` from `action_seq`; no transition log                     |
| m6 default-constructible token                    | Resolved: no default constructor, zero-token test (5.2)                  |
| m7 N above a tuned bound                          | Resolved in intent by R3-9 (rescale); see Lead rulings                   |
| m8 mirror rebuild at resume                       | Moot: no mirror (F23). The decision itself is contested in M2            |
| m9 latched tombstone holds budget for life        | Stated (6.5); the cost figure is still missing, see m6                   |

Details of the ones that needed a check:

- **M1/M2 (population, load bias).** Revision 3's design is the one I asked for: relayed outcomes
  count in both `R` and `N`, and a content-clean reading dropped for `IngressBackpressure` is a pass
  credit. With the credit, a 5% source at 4x overload stays NOMINAL (`mark.py`, rows "oracle" and
  "mark": SUSPECT share 0.000, mean `f` 0.050 to 0.051). The mechanism that decides "once" is what
  fails; see M1 below.
- **M3.** The lead chose classification at the check site; epoch regression and non-finite values
  are health refusals, `UncertainAcquisition` and mapping regression are the host time class (3.1).
  A device whose RTC resets (`StaleObservation`) or whose counter restarts (`StaleSequence`) still
  reads NOMINAL with every reading refused. That is the lead's decision and section 13 hands it to
  I3-3. I do not raise it again as MAJOR; m1 asks that the design say it.
- **m1.** Exact enumeration over all 2^12 refusal sequences (`nums3.py` section c): the start-up
  false-alarm rate in the first 12 events is 1.89% at a true 5% refusal rate and 10.74% at 10% with
  `min_events` 8 (design: 1.9%, 10.7%), and 13.03% and 29.92% at 3 (design: 13%, 30%). The first
  reading that can read SUSPECT is the ninth: `N` is 7.425 after 8 events and 8.266 after 9. The
  design's figures are right.
- **Table 3.3.** I recomputed all 18 cells with a vectorised first-order filter (`table.py`, 4x10^5
  events, seed 1, 3H warm-up). SUSPECT share and transitions per hour at 1 Hz: 10%: 0.2%/5.9,
  0.0%/0.2, 0.0%/0.0; 15%: 3.9%/76, 0.7%/12, 0.0%/0.5; 20%: 19.7%/252, 12.1%/129, 5.3%/47; 30%: 77%,
  86%, 94%; 50%: 100%. All inside the 2-point tolerance of the test. The 14-event and 64-event
  figures also check (`nums3.py` section b: `ceil(ln 0.75 / ln b)` = 14; `ceil(ln 0.25 / ln b)` =
  64).

## What is sound

- The pass credit answers the load bias. With the dedup made correct (M1), the estimator measures
  one population under load, relay topology and retransmission.
- `min_events` as a weight, with the cap at 90% of `1/(1-b)` and the `H >= ingress_queue_per_source`
  rule. At `H` = 8 the cap is 12.05 and the ceiling for `min_events` is 10; at 64, 92.8 and 83
  (`nums3.py` section a).
- Excluding the host time class from `f` is the right default while the clock mapping is the host's.
  A mapping outage must not make a fleet SUSPECT at once.
- The read API in 2a. An operator can now see state, deciding code, fraction and weight without
  reading arena cells, and the C++-equals-IR test makes disagreement a failure.
- Isolate withdrawing supports at once (A7), and `NoChange` without a turn. A retried isolate from
  an automation that lost its receipt costs nothing.
- Deleting the reflash runbook and stating A3. A device with no durable or fresh incarnation cannot
  be arbitrated; that is the true constraint.
- R3-9 (rescale `R` and `N` together on a shrunken `H`), R3-3, R3-4, R3-6 and R3-7, see Lead
  rulings.

## BLOCKING

None.

## MAJOR

### M1. The high-water mark of R3-5, as ruled, is raised by refusals and can freeze the estimator

(design 3.2; decisions R3-5; security M1, distributed M4; `agent_executor.cpp:3628-3640`,
`compiler.cpp:5361-5389`)

Problem. R3-5 keeps, per (actor, principal, incarnation, predicate), "the largest sequence that
moved the estimator", and counts refusals and pass credits only above it. A health refusal moves the
estimator, so a refused reading raises the mark. The sequence number is a signed field and `observe`
accepts any Int64 (`:3633`, `fits`). A health refusal is decided in `observe`, before the arena sees
the stream, so the arena's own stream floor (written at commit only, `compiler.cpp:5389`) does not
move. After one refused reading with a large sequence, the mark is above the arena floor and every
honest reading below it is skipped by the estimator while the arena commits it.

Evidence (`poison.py`, seeded; the device is a 30% bad source, one refused reading with sequence
2^31 at step 1000, 2,000 further steps):

| Rule                                           | Device after the poison                 | Result        |
| ---------------------------------------------- | --------------------------------------- | ------------- |
| R3-5 as ruled (mark raised by refusals)        | `f` frozen at 0.373, `N` frozen at 46.7 | SUSPECT, 100% |
| mark raised by commits only, ring for refusals | `f` tracks the device                   | SUSPECT, 85%  |
| R3-5 as ruled, device then fails (2% to 60%)   | estimator frozen at its healthy value   | SUSPECT 0%    |
| mark raised by commits only, same device       | follows the fault                       | SUSPECT 100%  |

The freeze is permanent until the device's sequence passes 2^31, so in both directions the display
is wrong for the life of the incarnation. A real cause is a device with a corrupted counter that
also emits a bad frame, the same population as the lost-counter case (r2 M3.4). A reset does not
help: it keeps stream floors (P19 2.1), and the ruling does not say whether it clears the mark.

Consequence. The fix for replay (the security and distributed finding) creates a new way for one
device-signed value to disable health for a source. The safe direction (frozen at NOMINAL while the
device fails) is the worse one for a hardware fault.

Required fix. The mark must never exceed what the arena would accept, and refusals must not raise
it.

- The mark rises on commits and on pass credits only. A pass credit is a reading that passed every
  check and that the arena would take; a refusal raises nothing.
- A signed-basis health refusal counts when its sequence is above the mark and not in a ring of the
  last `outcome_log` (64) counted refusal sequences for that stream. Replaying an archive of refused
  events then works only for events above the last accepted reading, which are events of the current
  outage; once the device commits one good reading, every archived event is below the mark
  (`poison.py` S2: SUSPECT share while a relay replays 1,000 archived bad events for 10^4 steps:
  evictable log 1.00; mark 0.00; commit-mark plus ring 0.00).
- Decide count or skip when the reading is observed and carry the decision on the queued item.
  Deciding at commit, against a mark a later credited reading has already raised, biases `f` up by
  about 10% (`mark_variant.py`, 30% source at 100/s offered, 50/s served: mean `f` 0.335 against
  0.306 for the oracle).
- State the lifecycle: the mark survives reset and unbind (it describes events, not health), is
  erased in `forget`, is per incarnation (a new incarnation starts at 0), and is lost with the host.
  Delete `Logged::counted`; the mark and the ring replace it, and two mechanisms for one fact is
  what R3-3 and security M1 both objected to.
- Tests: the poison case above (the estimator keeps following the device after one refused reading
  with a huge sequence), the 65-event and 1,000-event replay archive for 10^4 steps (`f` stays at
  the device's fraction), and the retransmission case below.

The retransmission case is why a bound on the *log* cannot be the answer even without an adversary.
With an honest device that resends dropped readings (3.2 assumes it does) and a 64-entry log, each
retry meets an evicted entry and is credited again: a 30% source at 100/s offered, 50/s served,
reads mean `f` 0.043 and SUSPECT 1.5% of the time against 0.306 and 88.7% for the oracle (`mark.py`,
rows "log64"). Revision 3's own mechanism hides a failing sensor under overload, so R3-5 is needed
for a hardware reason as well as the security one.

### M2. F23 (no latch mirror) rests on a premise the queue arithmetic does not support

(design 6.5, 10; decisions F23; `agent_executor.cpp:1999-2008`, `epistemic_limits.hpp:29-38`)

Problem. 6.5 says "the arena refusal allocates no cell, and the per-source queue share bounds a
flood". The share bounds one source at 8 queued readings. The actor's total queue is 64
(`kDefaultEpistemicIngressQueue`), so eight isolated, still-transmitting devices can hold all of it.
Draining is 4 ingestions per wake. A babbling device (the classic bus fault: a stuck transmit loop,
or firmware that retries without back-off) refills a freed slot within one inter-arrival time, so a
healthy 1 Hz sensor competes for each freed slot with probability about `λ_h / (Λ_babble + Σλ_h)`.

Evidence (`flood.py`; Poisson arrivals, 120 healthy sources at 1 Hz, drain capacity 200 readings/s,
the stated defaults for queue and share; capacity is my assumption and the results scale with the
ratio of offered to served):

| Isolated babblers | 20 Hz each: healthy readings accepted | 100 Hz each: healthy readings accepted |
| ----------------- | ------------------------------------- | -------------------------------------- |
| 0                 | 100%                                  | 100%                                   |
| 4                 | 99.9%                                 | 83%                                    |
| 8                 | 73%                                   | 29%                                    |
| 16                | 45%                                   | 12%                                    |

With a refusal at observe for a known-latched principal (a negative cache), the same runs accept
100%. The wasted turns are 77 to 186 of 200 per second in the loaded rows.

Consequence. A bad firmware rollout, which r2 M8 named as the fleet incident, is exactly eight or
more babblers. The operator isolates them, the isolate commits, and the actor still refuses most
healthy traffic with `IngressBackpressure`. The pass credit protects those sources' health, not
their data. The design's operator story ends with the flood still running, and 6.6's runbook advises
retire or unbind (6.5) without saying that this is the only way to restore service. This is the
"measured flood case" that F23 and section 10 say would reopen the decision.

Required fix. Keep the arena authoritative and add a host-side negative cache, not a mirror of
state: when `finishIngest` sees a `SourceFaulted` turn result for (actor, principal), record the
pair in `Ingress`; `observe` refuses a recorded pair with `SourceFaulted` before queueing. Remove
the entry when a source change, reset or health commit touches that principal, and on `forget`. It
is never persisted and is wrong only for the duration of one stale entry, which the next committed
change fixes; the first reading after a resume costs one turn. Charge nothing (the `chargeable`
exclusion already lists the code). If the lead keeps F23, the design must at least state the figure
above, the 2c2 runbook must say that isolating does not restore service when `k * share` reaches the
actor queue, and a test must show the healthy-source acceptance under eight isolated babblers.

## MINOR

- **m1. The NOMINAL blind spots are not named (3.1, 3.3).** The declared meaning in 3.3 is "about a
  quarter or more of the source's last few dozen outcomes were unusable". For a device whose RTC
  reset (`StaleObservation`), whose counter restarted (`StaleSequence`), whose mapped time is
  uncertain or whose handler wedges ingestion (`IngestionFault`, F11), 100% of outcomes are unusable
  and the state is NOMINAL. Say so in the Schema notes and 3.3 as a list ("the state measures
  refusals decided on the device's signed bytes and epoch only"), and make the 2a test for RTC-reset
  and counter-loss assert NOMINAL plus the lifetime counter and `lastRefusal`, so nobody later reads
  the test name as coverage. `SourceHealth` should expose the count of consecutive uncounted
  refusals since the last commit; it costs one host integer and lets an operator see "100% refused,
  NOMINAL" without I3-3.
- **m2. `InvalidAcquisition` is labelled host-mapped and counted anyway (3.1 table).** The rule at
  the top of 3.1 is "the bytes or clock the device itself produced", but the table's basis column
  says host-mapped and counts it for a direct copy. A NaN `acquiredAt` produced by the host's
  mapping is a common-cause fault (every source at once). Until I3-3 owns the mapping, either count
  it only where the adapter passes the device's own time unmapped, or add the r2 test: a mapping
  fault across 10 sources must not make any of them SUSPECT.
- **m3. The subnormal sentence is wrong (3.8).** "Subnormal `R` is legal: it decays to 0 after about
  34,000 further events." In IEEE-754 double with round to nearest, `x * b` rounds back to `x` once
  `x` is about `0.5 / (1 - b)` = 23.3 units of the smallest subnormal. From `R` = 1 the value stalls
  after 34,218 events at 1.14e-322 (23 units) and never reaches 0 (`nums3.py` section d). It is
  harmless to `f`, but a test that expects `R == 0` after a long clean run fails, and a host with
  flush-to-zero (some ARM builds and DSP targets) takes a different path, which matters for D1's
  cross-host tolerance. Correct the sentence; add `if R < 1e-30 then R <- 0` in the IR only if the
  owner wants exact zero.
- **m4. Closed charges post one note per refusal run (3.2).** With close-on-accept (R3-4), a 100 Hz
  source with refusal probability `p` posts about `100 * p * (1 - p)` notes per second: 21 at 30%,
  25 at 50%, at most 50 for strict alternation. Each is a bookkeeping turn (about 100 estimated
  cycles plus turn setup). That is at most 50% more turns for the worst alternating source. State it
  in the 2a price table and let the 2a gate cover a 100 Hz alternating source.
- **m5. Interim classification must be recorded for I3-3 (3.1, 13).** Section 13 hands I3-3 the time
  class, but not the observable symptom. Add: "a device with a bad clock or a lost counter is
  invisible to `f` until I3-3; its signs are `lastRefusal`, `contentRefusals` and no commits."
- **m6. The tombstone cost needs its figure (R3-1, 6.5).** A retired principal leaves about 10
  cells; a latched one about 82 (48 base, 14 fixed health cells, one 20-cell isolate entry). At the
  default ceiling of 24,576 cells, 2,457 bare or 299 latched tombstones fill it alone (`nums3.py`
  section f). A site that replaces 20% of 128 devices a year, isolating each failed one first,
  spends 128 tombstones in five years: 1,280 cells bare (5%) or 10,496 latched (43% of the ceiling).
  It reaches the ceiling in about 12 years latched, 96 bare; at 5% turnover 47 and 384. R3-1 states
  that tombstones are bounded only by the cell ceiling, so the figure belongs in the design, with
  the operator's only remedy (checkpoint, resume into a fresh actor, re-grant).
- **m7. Sizing a fleet at the defaults (R3-2).** With 48 base cells, 82 per record and the 142-cell
  health reserve charged per live profile, 24,576 cells hold: 90 sources of 1 record, 69 of 2, 47 of
  4, 29 of 8 and 16 of 16 (`nums3.py` section e); 128 live profiles at full reserve leave 768 cells,
  about 9 records for the whole fleet. A 100-sensor gateway with history 4 on one predicate (4
  records each) needs about 47 sources' worth of space per 24,576 cells, so it cannot run at the
  defaults. R3-2 already requires the plain statement; I ask that it include this table and the
  formula `sources x (48 + 82 x records + 142) <= cells`, and that O7 price a cheaper reserve (an
  isolate needs about 54 cells, not 142).
- **m8. `SettleFailed` and `IngestionFault` stay invisible to health (4.1, F11).** An ingestion that
  crashes the program's handler is refused for life and never charged. Part of m1; the
  operator-facing text should name `stat()`'s `refused.IngestionFault` as the sign.
- **m9. A host that restarts keeps no mark (R3-5).** The ruling accepts one replay pass after an
  executor restart. For a 1,000-event archive that is 1,000 counted events against the 14 needed to
  trip SUSPECT from clean (`nums3.py` section b), so a recorded outage replayed once after every
  host restart re-suspects the victim. Persist the mark in the arena as one Int per stream (it is
  the same number as the commit mark, which the stream already stores as `lastSeq`), or state the
  replay bound as "any archive of 14 or more events, once per restart".

## Lead rulings

- **R3-1 (tombstones charged what they hold; exempt headroom for actions on them).** Sound, and
  right for sensors: devices are replaced, not repaired in place, and a charge that grows with every
  replacement would end the actor. Revision 4 must give the figure (m6), say that a latched
  tombstone is the expensive one (about 82 cells) and that the headroom `cells / 16` = 1,536 cells
  covers about 18 isolate-created or acted-on tombstones before `EpistemicBudget`, so a batch
  isolate of 100 devices that were never bound cannot all succeed.
- **R3-2 (capacity relations become a construction-time WARN).** Sound. Revision 4 must publish the
  sizing table and formula (m7). The WARN should print the formula's left and right sides so an
  integrator sees the shortfall.
- **R3-3 (`firstCount` gates the estimator only).** Sound, and it removes the conflict with the
  pinned lifetime counters. With the corrected mark of M1, `firstCount` is the mark test, and
  `Logged::counted` is deleted.
- **R3-4 (a closed charge moves into its note).** Sound. State the bound: one open charge per
  (actor, principal) and at most 9 closed ones queued per principal (8 queued accepts plus one), and
  what `abandon`, `dropHostEvents` and a superseded note do with them (they add to the lifetime
  counters; they do not move `R` or `N` after a reset). Add the note rate of m4 to the 2a gate.
- **R3-5 (high-water mark).** The right shape and the wrong trigger; see M1. It also needs a
  lifecycle statement and the persistence option of m9.
- **R3-6 (reset floor from readings in flight).** Sound and better for hardware than the first
  design: a device with a corrupted counter that is refused cannot poison the reset. It leaves the
  mark of R3-5 and the in-flight multiset as separate structures with different jobs; revision 4
  must say so and must not reuse one for the other.
- **R3-7 (strict control priority, cap yields to other actors only).** Sound. Consequence to state:
  while a capped control run is in progress, ingestions for that actor wait and the per-source queue
  fills at 8 readings, 80 ms of data from a 100 Hz sensor. Dropped readings are
  `IngressBackpressure`, retryable, and credited, so health is not harmed; data is lost unless the
  device retries. Say that a device that does not retry loses those readings.
- **R3-8 (A4 reworded; wedge test in 2b).** Sound from my side. A source whose garbage crashes the
  program's handler stays NOMINAL (m1, m8); say so next to A4.
- **R3-9 (rescale `R` and `N` together).** Sound. It preserves `f` (a mature source at `f` = 0.126
  would otherwise read 0.251 after H 64 to 32, epistemology M3). `min_events` is unaffected because
  the test uses `N` and the validated ceiling is 90% of the new cap. Revision 4 must say the rescale
  also applies to a resumed arena, and that `sourceHealth` shows the rescaled `N`.
- **R3-10 (audit sink contract).** Sound for the interface. One hardware consequence: the sink
  cannot be more durable than the arena. A record handed to the sink after commit is lost if the
  host crashes before the arena is checkpointed, and the isolate itself is lost with it (6.7). If
  the owner answers "no durable record, no isolate", the host must checkpoint before it reports the
  action done; `checkpointed` in `HealthOutcomeView` already supports that.
- **R3-11 (no shape or state version in I3-2).** No hardware objection.

## Questions for the owner

1. **Latch cache (M2).** Is a host negative cache of latched principals acceptable if it is never
   persisted and never authoritative, or do you want F23 kept and the flood figure stated?
1. **Mark persistence (m9).** Should the estimator's high-water mark live in the arena, where it
   costs one Int per stream and survives a restart, or stay host memory with the replay bound
   stated?

## Verdict by slice

- 2a: FAIL (M1; the estimator's dedup is in this slice).
- 2b: PASS (no hardware finding).
- 2c1: PASS (no hardware finding; R3-7's consequence is stated above).
- 2c2: FAIL (M2; m6 and m7 belong to its budget text).

Tally: 0 BLOCKING, 2 MAJOR, 9 MINOR.

Verdict: FAIL
