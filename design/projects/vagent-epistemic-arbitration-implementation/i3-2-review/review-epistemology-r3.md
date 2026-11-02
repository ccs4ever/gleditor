# I3-2 design review, round 3: epistemologist (persona epistemology)

Scope: `i3-2-design.md` revision 3 (uncommitted, 1,136 lines) with `decisions.md` section 1 and 3
and `i3-2-review/findings-r2.md`, against P19 v1.5 sections 2.1 (preservation), 4
(`EpistemicHealthGrant`) and 5.1 (latch, reasons, displayed state), the owner's Q1-Q3 rulings and
the I3-2 additions. Code at HEAD `8a4089d9` plus the uncommitted T0 change. Persona: what the system
claims to know, and on whose authority. Scratch: `/tmp/p19-epistemology/r3.py`, `r3b.py`, `r3c.py`.

## Earlier findings

| Round 2 | Status   | Why                                                                                                                                                                                                                                                                                            |
| ------- | -------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1      | resolved | 6.4 writes `reset_epoch` into each new record's `calibration_epoch` (today the constant 0, `agent_executor.cpp:3734`) and `retired_by` (five values) on every retired record. Both are pinned by the ABI test (section 7). A pre-reset label now keys differently from the reset learner's.    |
| M2      | resolved | 6.5 evicts by class (an ordinary entry never evicts an isolate or reset entry), merges repeats into `count`, keeps the live fault entry, and copies the removed fault into the reset entry's `removed` text. The audit sink (F28) is the durable record. Residual wording errors are m3 below. |
| M3      | resolved | 5.2 enforces the ban at `grantHealth`, at `bindSource` and revive, and at turn time, host-wide, on canonical tokens. Both orders are tested. `everBound` growth is m6 below.                                                                                                                   |
| M4      | resolved | 4.1 and 6.6 add `slotReserve = arenaSlots / 64` for settle and health entries, a hard second limit, a measured test and a runbook. The reserve's upper bound is not validated (m7) and the runbook's step order is wrong (m8).                                                                 |
| M5      | resolved | 6.1 adds `origin` to the reason, reserves one slot for evidence-driven reasons, makes `Clear {}` operator-only, and tests a `device` reason surviving it. Amendment A5 records it.                                                                                                             |
| m1      | partial  | 3.4 adds a licenses paragraph, but "NOMINAL: nothing withheld" is false for a source whose readings are all refused as host time class (m1 below).                                                                                                                                             |
| m2      | resolved | `min_events` 8 as a weight; I recomputed `N` after 8 and 9 events as 7.425 and 8.266, and the cap `floor(0.9/(1-b))` as 42.                                                                                                                                                                    |
| m3      | resolved | `SourceFaulted`, `SupersededByReset`, `SettleFailed` join the `chargeable` exclusions (7, 4.1); the latch check comes first (6.5); the relayed-copy test is listed.                                                                                                                            |
| m4      | resolved | `health_fraction` is stamped; stamps are declared not replay-deterministic (3.4).                                                                                                                                                                                                              |
| m5      | resolved | `refusedWhileFaulted` is host-side and shown in `SourceHealth` (6.5, 6.7). A last-contact time is not kept, which is enough for the operator's question.                                                                                                                                       |
| m6      | resolved | (a) the generation commits with the reset (5.4); (b) the arena stream floor refuses a resend (6.4); (c) dropped counts reach the lifetime totals (6.4); (d) recorded as a limit (6.4, 13).                                                                                                     |
| m7      | partial  | Discards report counts and up to four event-type names, not tuples, and there is no re-drive (m5 below).                                                                                                                                                                                       |
| m8      | resolved | Operator codes cannot use `refusal_fraction`, `rejection_streak` or `fault` (5.3); one reasons slot is reserved.                                                                                                                                                                               |
| m9      | resolved | Note fields `health` and `passed` and a third count are in 3.2 and 3.8.                                                                                                                                                                                                                        |
| m10     | resolved | Decision F1(b): relayed outcomes count once per (incarnation, event ID).                                                                                                                                                                                                                       |
| m11     | partial  | Most tests are listed. Missing: a refusal-only profile reading SUSPECT and then reset, and a fleet-wide frame-policy change that heals (m9 below).                                                                                                                                             |
| m12     | resolved | `UncertainAcquisition` is host time class (decision F5(A)); the cost is m1 below.                                                                                                                                                                                                              |

## What is sound

- **The estimator numbers reproduce.** I recomputed the simulation table of 3.3 (4x10^5 Bernoulli
  events, seed 1, warm-up 3H): every cell matches to the printed precision (for example 20% at H =
  32 gives 12.0% SUSPECT and 132 transitions an hour). The start-up false-alarm rates match (1.9%
  and 10.7% at `min_events` 8; 13.0% and 29.9% at 3), as do 14 events to SUSPECT from a clean
  history, 31 for an alternating source, 64 good readings back to NOMINAL, and the mean `f` of 0.200
  at a 20% defect rate.
- **Isolate as withdrawal of testimony.** 6.2 states that the drop makes no refutation, label or
  calibration move, and `retired_by` makes the cause readable afterwards. That is the licence I
  asked for in round 2.
- **Reset is bounded by evidence, not by a call.** The `expectResetEpoch` check, the commit-time
  generation and the arena stream floor together mean a retried or revoked reset destroys nothing
  and a resend cannot return. This is a clear improvement on revision 2.
- **Per-event settle turns** stop one failing handler from discarding its siblings' events, and the
  outcome is published once, still `Committed`, so a caller has no reason to retry a change that
  happened.
- **The two-consumer rule** (stamp for audit, query-time `rtHealth` for eligibility) and the
  provenance-bearing `reasons` and `actions` structures meet P19 2.1's "preserve ... provenance".
- **The amendments list** (section 11) names every departure with its source. A1 to A3 and A5 to A10
  match the code and the design body.

## BLOCKING

None.

## MAJOR

### M1. Amendment A4 says an operator isolate or reset "unwedges" a failing ingestion handler

Problem. Section 4.1 ("Ingestion keeps the wedge") and A4 state that a program whose handler for an
arbitrated predicate always fails cannot ingest that predicate, and that "an operator isolate or
reset unwedges the source". Ingestion still ends in `rtSettle_` inside the same all-or-nothing turn
(`compiler.cpp:5668`; the design cites the same line). An isolate or reset drops every support, so
the next reading for a key finds no current record and raises no `-old` event; it commits. The
reading after that replaces a current record, raises `-old` for the changed tuple, and the always
failing handler undoes the turn again (`IngestionFault`, `NeedsHostAction`).

Evidence. The mechanism is the design's own (4.1 and the F11 text: "a reading that replaces a key
and raises `-old` ... is refused for ever"). Nothing about the repair state keeps the failure from
recurring: the handler, not the source, is the cause. The test list claims "the I3-1 ingestion wedge
test unchanged" (4.3), but no such test exists at HEAD:
`grep -rn "wedge\|IngestionFault" tests/xuzz` finds only a counter in
`vagent_epistemic_noise_test.cpp:158`. The residual therefore has no test that pins either the wedge
or the claimed remedy. I did not build a probe (the lead owns builds).

Consequence. The owner ratifies A4 and the Schema notes and program documentation repeat it. An
operator who isolates or resets a source in response to `IngestionFault` loses the source's whole
belief set (A7) and buys one reading. The next update refuses again, and each repetition costs the
operator another full withdrawal. The sentence tells the operator the wrong remedy. The true remedy
is to fix the handler (an upgrade).

Required fix. Reword A4, 4.1 and the Schema text: the key cannot update until the handler is fixed
by an upgrade; isolate or reset admits at most one non-replacing reading per key and is not a
remedy. Name the signal (`IngestionFault` count in `stat()` and the refusal class). Add the missing
test, in 2b: an always-failing `-pred` handler, a replacing reading refuses, a reset, one reading
commits, the next replacing reading refuses again. If the owner wants a real remedy, the option is
the one decision F11 declined (settle after commit for ingestion); put that to the owner as a
question with this evidence (below).

### M2. The health reserve is charged to tombstones, which are never removed

Problem. 6.6 charges the reserve `healthBound - health_cells` for "every existing profile,
tombstones included". A retire leaves the profile as a tombstone and nothing removes one: the
epistemic cell walk and the code that sets `retired` (`compiler.cpp:4940`, `5139`) have no excision
path, and tombstones keep streams and dedup that refuse replays. Only the count of *live* principals
is bounded (`compiler.cpp:4720`, `4992`). Ordinary admission is
`cellsWithin(all) + sum(reserve) <= cells`.

Evidence (`r3c.py`, defaults: 24,576 cells, base 44, `healthBound` 142). A tombstone costs 44 cells
at HEAD and 186 in revision 3. The ceiling is exhausted by 558 retired principals at HEAD and by 132
in revision 3, with no live source and no reading stored. The exempt cap "2 x max_sources" profiles
needs 47,616 cells against a hard ceiling of 26,112, so the cap is unreachable and the reserve sum
passes `cells` first. The `validate()` rule `max_sources x (healthBound + base) <= cells` covers 128
profiles only (23,808, leaving 768 cells, about nine records).

Consequence. After 132 retirements (a fleet of 128 devices replaced about once, in total) every
ordinary admission refuses `EpistemicBudget`: no reading from any live source is stored and no
`Degrade` is accepted, and no operation recovers it because none removes a tombstone. A tombstone
cannot use its reserve: an unbound principal's health actions end `NoProfile` unless it has a
profile, and a tombstone holds the health state it already has. The design's claim that "the cost
follows use" is false for the one profile kind whose use has ended.

Required fix. Charge the reserve only to live (non-tombstone) profiles; charge a tombstone only the
`health_cells` it holds. Let a health action on a tombstone use the exempt headroom (`cells / 16`).
Make `validate()` and the cap arithmetic honest: either bound tombstones (state a maximum and refuse
the retire past it with a typed code) or state that tombstone count is a lifetime budget and give
its number. Test: 200 bind/retire cycles with the budget at its default, then an ingestion commits.
This slice is 2c2; the tombstone count at HEAD is a defect of I3-1 that 2c2 makes four times worse.

### M3. Clamping `N` and then `R` on a shrunken `H` changes the fraction

Problem. 3.2: after each update `N <- min(N, 1/(1-b))` and `R <- min(R, N)`, so "a shrunken `H`
takes effect at the next update" (m10, A2). The clamp lowers `N` and leaves `R`, which doubles `f`
when `H` halves.

Evidence (`r3c.py`). A mature source at `N` equal to the old cap and `f` = 0.126 reads 0.251 after H
64 to 32, and the same source reads 0.249 after H 32 to 16. The flip point for H 64 to 32 is `f` =
0.126; for H 128 to 32 it is 0.063. Scaling both gives the unchanged `f` in every case. The fuzzer
invariant in 6.8 ("`R <= N <= 1/(1-b)` against the *largest* bound in force") passes the distorted
pair, so no test would see it.

Consequence. A fleet-wide setting change (or a resume under new settings, A2 item 3) sets every
source with `f` above about 0.126 SUSPECT at once, against the declared meaning "about a quarter or
more of the last few dozen outcomes were unusable" (3.3). The state lasts until traffic decays `R`,
roughly `H` events, and I4 will inflate and stamp with it. The ratified amendment (A2) would carry
the defect.

Required fix. Rescale: when `N` exceeds the new cap, set `R <- R x cap/N` and `N <- cap`, preserving
`f`. State it in 3.2 and A2. Test: `f` is unchanged by a clamp, over several `H` pairs; a source at
`f` = 0.15 stays NOMINAL across H 64 to 32.

## MINOR

- **m1. "NOMINAL: nothing withheld" is false for host-time-class refusals.** Decision F5(A) keeps
  `StaleObservation`, `ObservationTimeMismatch`, `UncertainAcquisition` and `ClockMappingRegression`
  out of `f`. A device whose every reading is refused for a bad clock reads NOMINAL. Reword 3.4:
  NOMINAL means no *counted* adverse evidence; it says nothing about liveness, accuracy or host-time
  refusals. Show the lifetime time-class refusals and a recent-window count in `SourceHealth` so the
  operator sees the case. Also reword the declared meaning in 3.3 to "of the last few dozen
  *counted* outcomes".
- **m2. The pass credit counts an event the arena never judged.** 3.2 counts a reading dropped under
  load as accepted. `ClockEpochRegression` is decided in the arena (a health refusal), so under
  backpressure a source with a reset RTC is credited instead of charged, and a credited event that
  the arena would have refused for that code is never charged (`counted`). Record the limit (one
  code today) and name it in A8. The cited figure "f peaks at 0.78 against 0.06 with the credit"
  does not reproduce: a 5% source over 1,000 events peaks at 0.11 to 0.21 with every event counted
  (`r3b.py`, seeds 1 to 5), and 0.84 to 0.93 without credit in my model. The test
  `HealthEstimatorSimulation` should assert "stays below `suspect_fraction`", not 0.06.
- **m3. Audit bookkeeping.** `actions_evicted = action_seq - entries stored` (6.5) counts merged
  repeats as evictions; use `action_seq - sum(count)`. An ordinary entry is "not stored at all" when
  the list holds only safety entries; say so beside the bound, so an operator does not expect
  `degrade` history in the arena. A second operator's `Isolate` on a latched source is `NoChange`
  and reaches neither the arena nor the sink; state that the caller's own record is the only trace.
  `removed` is capped at 1,024 bytes (6.1) and 16 reason codes of 64 bytes already fill it; define
  the truncation.
- **m4. `wouldChange` for `Reset` (6.3).** "A reset of a profile that is clean ... is `NoChange`"
  must include `R = N = 0`, no liveness stamps and no reasons; a SUSPECT-by-fraction profile with no
  current record still changes on reset. Say so, and test it.
- **m5. Discarded settle events (4.1).** `names` holds four event types with no overflow rule and no
  tuples, and nothing re-drives a discarded `-pred`. After an isolate of a faulty source, a program
  whose handler failed still holds the withdrawn belief, which is the case the safety action exists
  for. Record that as a known limit in 7.5/A9 and say what the program can do (its own re-query);
  state the overflow rule (count past four, `names` truncated).
- **m6. `Ingress::everBound` (5.2) is unbounded host memory** (64-byte tokens, never erased). It is
  host-controlled, so bound it (a ring with a stated size, or erase on `forget` for principals with
  no profile anywhere) or record the growth rate. An issuer bound on a different executor, or before
  a restart, is covered only by the bind-time check; say so.
- **m7. `slotReserve` has no upper validation.** `arenaSlots + arenaSlots/64` stays below
  `ephemeralBit` (2^31, `manifold.hpp:154`) at the default (`2^30 + 2^24`) but not at
  `arenaSlots = 2^31 - 1`; `validate()` should require the sum below the bit, or the allocation
  limit fails mid-turn in exactly the regime the reserve protects.
- **m8. Runbook order (6.6).** "Isolate the flooding source, then checkpoint" cannot work:
  `checkpoint` refuses `HostDeliveryPending` while the post box holds items
  (`agent_executor.cpp:4956`), and 6.5 itself says an isolated flooder keeps the queue non-empty.
  Put "unbind or retire the flooder, wait for the queue to drain" before the checkpoint, and make
  the listed runbook test do exactly those steps.
- **m9. Missing tests.** A refusal-only profile that reads SUSPECT and then resets (3.4 states the
  rule); a fleet-wide frame-policy change in which all sources become SUSPECT and heal (round-2
  m11); the reset's stream-floor creation counted in `kProfileBaseCells`/`healthBound`
  (`healthBound` of 142 cells omits streams that a reset creates for predicates with admitted but
  uncommitted readings).
- **m10. The reset floor is taken from `admittedSeq` (5.4), not from what was superseded.** A
  reading that passes `observe` and is then refused in the arena (for example
  `ClockEpochRegression`) or dropped under load raises `admittedSeq`; a reset then raises the stream
  floor to that sequence. A device that emitted a corrupt counter value alongside a clock fault
  would be locked out of the predicate by the very reset that repairs it, since streams survive
  resets and only a new incarnation clears them (A3). Take the floor from the readings actually
  superseded plus the committed stream value, or record the lock-out as a limit.
- **m11. `ConflictingEvent` is origin equivocation.** The digest covers only signed fields
  (`agent_executor.cpp:3676-3688`), so two different digests under one event ID were both signed by
  the origin. That is better evidence against a source than a frame mismatch, and it is not in the
  counted table. Count it as a signed-basis health refusal (relay-safe) or say it is diagnostic.
- **m12. Two implementations of the state rule.** `sourceHealth` derives the state in C++ and the
  arena derives it in IR (3.5). The sweep test covers every state; add a differential test at the
  threshold edges (`f` exactly at `suspect_fraction`, `N` exactly at `min_events`, subnormal `R`) so
  a disagreement cannot hide between states.
- **m13. The wedge test cited in 4.3 does not exist** (see M1); write it rather than cite it.

## Questions for the owner

1. Ingestion wedge (M1). With the remedy corrected, the residual means an always-failing `-old`
   handler blocks a key until the program is upgraded. Keep decision F11(a), or settle after commit
   for ingestion as well, which breaks the I3-1 all-or-nothing contract but removes the wedge?
1. Tombstones (M2). Should a retired principal ever be reclaimable? A tombstone's streams and dedup
   are the replay protection for its old incarnations, so reclaiming needs a rule (for example,
   reclaim only after `max_age` plus a floor on the stream).
1. Should `ConflictingEvent` count against a source's health (m11)? I recommend yes, signed basis.

## Per-slice verdicts

- 2a: FAIL (M3). The estimator, classification, stamp and read API otherwise stand.
- 2b: FAIL (M1). The settle contract itself is sound.
- 2c1: PASS. The trust boundary, the three-point self-certification rule, the token, grant and
  receipt model, and the stub turn meet the earlier findings; m6 and m8 are minor.
- 2c2: FAIL (M2). The audit, idempotency, reset markers and class eviction are sound.

Tally: 0 BLOCKING, 3 MAJOR, 13 MINOR.

Verdict: FAIL
