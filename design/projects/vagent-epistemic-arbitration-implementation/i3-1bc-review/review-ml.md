# ML gate review: P19 I3-1b and I3-1c

Scope: `git diff 00db472b..4ad3f53a`, judged as the data pipeline for I4/I5 scoring, calibration and
fusion. Files read: `compiler.cpp` (`emitObserve`, `emitObserveRefusal`, `emitSourceChange`),
`agent_executor.{hpp,cpp}` (`Ingress`, `observe`, `bindSource`, `retireSource`, `drainPosted`,
`deliverHost`), `epistemic_ingress.hpp`, `epistemic_limits.hpp`, `runtime_abi.hpp`, the ingress and
noise tests, `noisy_sensor_sim.hpp`, `epistemic_invariants.hpp`, the fuzzer and its corpus, against
`i3-design.md` sections 3-6, 8, 9, 11, 13. No repository file was changed. All probes and mutants
live in `/tmp/i3-1bc-ml/` (a gtest file and one-file mutants linked against the existing objects;
repo-root strays of my own were removed; the untracked `crash-deffbbef...` at the repo root predates
me and now replays clean).

## Probe results

Existing suites (headless, `build/vagent_actor_test`): `VagentEpistemic*` 67 tests pass. The noise
test passes at 1, 3 and 12 seeds (3 seeds: Committed 295, Duplicate 66, IngressBackpressure 51,
InvalidVariance 79, ObservationTimeMismatch 4, IngestionFault 0). The corpus replays through
`fuzz_epistemic_ingress` (my non-sanitised build of the same harness) to 16 outcome codes.

Probes (`/tmp/i3-1bc-ml/probe.cpp`):

| probe                                                              | result                                                                                              |
| ------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------- |
| keyless continuous predicate `pos(x: Float)`, 8 readings           | readings 1-4 commit; 5-8 `EpistemicKeyLimit`; 4 clauses, 4 supports, **4 current** records          |
| exact retry of a committed event after `max_age`                   | outcome `Committed` becomes `Refused/StaleObservation` (Terminal); `content_refusals` 1; contact set |
| relayed copy with another tuple, same event ID, after the commit   | original's outcome becomes `ConflictingEvent`; `refused_count` 1                                    |
| epoch 5 reading, then epoch 4 reading 5 s older, then lower mapping | all three `Committed`; the epoch-4 reading retires the epoch-5 one; the lower mapping one is current |
| 300 refused readings from one principal, 256-entry outcome log     | another principal's committed outcome: `nullopt`                                                    |
| 5 bad first contacts from a new principal, then a good one         | no profile; after the good one `content_refusals` is 0                                              |
| coalesced refusal note, a commit queued between two refusals       | `last_contact` ends at the commit's earlier receipt time, below the later refusal's contact          |
| relayed copy first, direct copy second                             | direct copy is `Duplicate`; `last_contact` stays unset                                              |

Mutation check (33 one-line mutants of `compiler.cpp` / `agent_executor.cpp`, run against the
ingress + noise tests, and the corpus replay as the fuzz stand-in):

| mutant                                                           | killed by tests | killed by fuzz | killed by noise |
| ---------------------------------------------------------------- | --------------- | -------------- | --------------- |
| advance retires wrong side; sequence `<=` to `<`; conflict as duplicate | yes       | advance only   | advance only    |
| acquisition-order inverted; trim never; previous current not retired | yes        | no             | no              |
| retire skips excision / support drop; revive keeps tombstone; tombstone counted | yes | no             | no              |
| advance keeps dedup/streams                                      | yes             | no             | no              |
| relay refreshes contact; content counted as other; refused_count dropped | yes     | no (one: relay-charge) | no      |
| relay ignored in charge                                          | yes             | yes            | no              |
| backpressure charged; digest includes relay / drops tuple; frame check off | yes  | no             | relay digest only |
| basis / wall-sync / incarnation / alias / queue-share checks off  | yes             | no             | no              |
| **variance min inclusive, variance max inclusive**               | **survives**    | survives       | survives        |
| **`max_age` without uncertainty term; future check without skew** | **survives**   | survives       | survives        |
| tombstone floor not raised in retire; trim spares-current check removed | survives (equivalent: the advance turn already raised the floor; the newest record is the only current one) | | |

The unit tests carry nearly all the weight; the property test and the fuzzer each kill two or three
of 33 and never the counter, retire or dedup mutants.

## BLOCKING

None.

## MAJOR

**M1. A keyless continuous predicate silently freezes its source and leaves several current
records.** `emitObserve` (compiler.cpp:5060-5096) bounds a principal at `keys_per_source` supports,
and a keyless predicate makes every distinct tuple its own clause and support (compiler.cpp:4690-4694
comment, `nkeys == 0`). The checker accepts `predicate pos(x: Float) arbitrated by truest(...)`. Probe:
readings 1-4 commit as four clauses and four *current* records for one principal; every later reading
is `EpistemicKeyLimit` (Terminal, "never resend") until the operator retires the source. A learner
then sees four simultaneous "current" values from one sensor, none superseding another, and the
sensor's real newest value never arrives. The invariant "at most one current record per (principal,
key)" is vacuous here: `EpistemicView::keyOf` assumes the first argument is a key and the fuzzer and
noise programs both use keyed predicates only. Remedy, either: refuse a continuous arbitrated
predicate with no `key` parameter in the 1a checker (a continuous quantity needs an entity to be a
measurement of), or give a keyless predicate a single implicit key so a new reading supersedes the
last. Add a keyless program to the fuzzer and the property test.

**M2. Observe-time content checks run before dedup, so a retry or a buffered flush is mislabelled
and mischarged, and refusals overwrite a committed outcome.** `observe` (agent_executor.cpp:3137-3245)
refuses `StaleObservation`, `ObservationTimeMismatch`, frame and variance on the bytes alone, and
`refuse()` always appends a new outcome entry (`Ingress::log`, :658) and charges the principal
(`Ingress::charge`, :677). `ingestOutcome` returns the newest entry. Consequences, probed:

- An exact resend of an event already committed, after `max_age` (a reconnecting host, a store and
  forward sensor flushing its buffer), turns the event's outcome from `Committed` into
  `Refused/StaleObservation`, class Terminal ("never resend these bytes"), while the record is in
  the belief set. The same resend also bumps `content_refusals` and refreshes `last_contact`
  (`isContentRefusal` includes `StaleObservation`, and a direct refusal "shows the device alive"),
  so a healthy source's retry counts as a content fault and as liveness. The design's replay horizon
  ("the window is the horizon") is therefore really `min(window, max_age)`.
- A relayed copy of a committed event with different bytes flips the original's logged outcome to
  `ConflictingEvent`. A relayed `Duplicate` is the case the lead knows; this is the worse one: any
  relay, or `IngressBackpressure` on a retry, rewrites the outcome the host's labelling and retry
  logic read.

Remedy: make the outcome log monotone per (principal, incarnation, event ID): once `Committed`,
later entries for the key are recorded as retries (or ignored), never replace it. Do not charge
content counters for an observe-time refusal whose key the log already shows `Committed` or
`Duplicate`, or defer the age and skew checks into the turn after dedup. A test for each.

**M3. The recorded observation omits the fields the design's section 6.6 lists, and nothing records
the deferral.** `emitObserve` builds the record from `callCopy(obs, rec)` plus `status`, `verdict`,
`verdict_epoch`, `verdict_revisions`, `pinned` and the profile view (compiler.cpp:5006-5016). Absent:
observation version, domain/schema ID (predicate signature with the policy hash), `model`,
`model_epoch`, `calibration_epoch` (so "calibration epoch 0" is not stored at all, only the string
`UNTESTED`), `feature_schema`, the empty feature list, and **health at ingest**. Also absent: the
executor-clock-instance tag that section 4 requires on steady times (`received_steady` is a bare
process-relative double). None of the commit messages or the ABI comments mention a deferral. These
cannot be backfilled: history excision (default 4 per support) and retirement delete the records
that would need them, health at ingest is a moment, and after a policy change nothing says which
policy a record was ingested under. What a scorer needs from each record that is present is right
(time, uncertainty, units, frame, variance, sequence, relay, both receipt times, incarnation,
event ID, digest, status), and the unscored default is clear (`UNTESTED`, no calibration record),
but the identity fields are what makes a stored reading usable for calibration across versions.
Remedy: add the fields as typed cells at ingestion (constants are fine, the point is that the record
carries what produced it), or amend the design and ABI to state that I4 adds them with a version bump
and that records without them are legacy and ineligible. Add the clock-instance tag or state it as 1d's.

**M4. Clock epoch and mapping version are stored but never compared; a lower epoch bypasses the
acquisition-order guard.** Section 6.4 requires the stream to hold the last sequence, epoch and
mapping version, and to refuse a lower epoch (or lower mapping version within an epoch)
`ObservationTimeMismatch`. The stream stores only the sequence (compiler.cpp:4885-4895). Worse, the
per-key ordering step treats *any* differing epoch as "new epoch, resets the comparison"
(compiler.cpp:5030-5052: `Eq epoch? ... requireTrue(same, retire)`), so an older reading marked with
a lower epoch retires the newer current record. Probe: epoch 5 reading, then an epoch 4 reading
acquired 5 s earlier, both `Committed`, the older is current. Only `max_age` bounds the damage, but
"current" is the first thing every scorer reads. The epoch and mapping fields are host-trusted, so
this is a defence-in-depth gap, yet the design priced and promised it and the 1b test list implies
it. Remedy: store epoch and mapping version on the stream, refuse regressions in the turn, and let
only a *higher* epoch reset the per-key comparison. Tests for epoch decrease, mapping decrease and
epoch increase.

## MINOR

1. **Outcome log is one ring per actor** (agent_executor.cpp:658-662, 256 entries). Probe: 300
   cheap refusals from one bound principal evict another principal's committed outcome, and
   `ingestOutcome` then returns `nullopt`, the same answer as "never seen". Observe-time refusals
   consume no queue slot, so nothing limits the flood. Give each principal its own sub-ring, or
   return a distinct "evicted" status.
1. **First-contact refusals leave no trace in the counters.** Probe: five bad first events from a
   new principal create no profile (`NoProfile`), and `content_refusals` stays 0 after the first good
   one. A source that is wrong from the start looks cleaner than one that was right and then slipped.
   Accepted by the design (nothing to charge); I3-2 must say how health treats a source with no
   profile but a refusal history in the log.
1. **Host-caused refusals reach `refused_count`.** `chargeable` (agent_executor.cpp:699) excludes
   only the three named conditions, so `EpistemicBudget` (a host ceiling), `UnboundIncarnation`
   (the host has not rebound yet) and the drain-time `StaleIncarnation` of a superseded queue entry
   (:3411-3425) are charged to the source. Diagnostic only today; if I3-2 reads `refused_count`
   these bias against rebooting sources. Exclude them or document them as host conditions.
1. **`last_contact` is not monotone.** Charges coalesce and carry the *latest* contact while their
   note sits at the *first* refusal's place in the post box (agent_executor.cpp:677-690, 3363-3371);
   a commit queued between runs after the note and lowers `last_contact` (probe: contact ends at the
   commit's earlier receipt). `emitObserveRefusal` (compiler.cpp:4464-4467) should write the max.
1. **Relay-first then direct leaves a live device uncontacted.** Probe: the direct copy is
   `Duplicate` and refreshes nothing, so a device whose copies reach the actor via a relay first
   shows no contact while healthy. By design and assigned to I3-3; keep it on that list.
1. **Counters are lifetime-cumulative across incarnations and revivals.** The retire turn keeps
   the whole profile (counters, latches, last times) as the tombstone, not the "about ten cells"
   the design states, and revive reuses it with no incarnation tag on the counters. The design
   wants latches to stay; I3-2 needs per-incarnation or windowed counters, and the tombstone size
   in the design should be corrected.
1. **Boundary mutants survive**: `min_variance`/`max_variance` inclusivity, the uncertainty term in
   the `max_age` test and the `clock_skew` term in the future test. Add exact-boundary cases
   (variance equal to each bound, acquisition exactly at `receipt - max_age - uncertainty`,
   `receipt + uncertainty + skew`).
1. **The property test's evidence is structural only.** `noise_test.cpp` checks `violation()`,
   typed outcomes and "no committed outcome means no belief change". It never checks that a
   committed reading's recorded fields equal what was sent, that counters equal the number of direct
   content refusals the simulator produced, that relayed copies add no record, or that reports
   after a reboot and before the rebind are refused as designed. `truth()` is never used, and
   reports carry no ground-truth label for I4 to reuse. Simulator coverage gaps: one key
   ("target"), so key limit, transfer by key and keyless predicates are untouched; no
   `StaleSequence`, `StaleObservation`, `ConflictingEvent`, `UnboundIncarnation`,
   `ClockUnsynchronised`, `EpistemicKeyLimit` or `EpistemicBudget`; relayed copies are emitted
   immediately, so a pre-reboot copy arriving after the rebind (the replay the incarnation exists
   for) is never produced. A cheap, strong addition: assert per-principal `content_refusals` and
   `refused_count` against tallies of the outcomes the test saw.
1. **The noise test depends on real time.** The simulator stamps acquisition on wall seconds
   starting five seconds before the run and the executor uses the real wall clock for receipt, with
   `max_age: 10s`. At -O3 a seed takes about 1.3 s; on a slow or sanitised build (10x or more) later
   readings pass `max_age` and turn into `StaleObservation`, changing the outcome mix. Use
   `clock: "actor_mapped"` against the executor's steady clock, or an injected clock, so a seed is the
   same run everywhere. The header's claim that a seed means the same run with every standard library
   also overreaches: Box-Muller uses `std::log` and `std::cos`, which libm implementations round
   differently in the last bit. The distributions are otherwise reasonable and honestly stated;
   odometry's reported variance grows geometrically (`1.15^n`) while its true drift variance grows
   linearly (`0.01 n`), which is fine as a stated assumption but means I4 must not treat it as
   calibrated.
1. **Fuzzer evidence is thin in four ways.** (a) The corpus reaches 16 outcome codes but not
   `EpistemicKeyLimit`, which the design's "covers each refusal code" requires, and not a keyless
   predicate. (b) "`$epistemic_cells` equals a walk of the graph" (section 11) is not checked and is
   moot: the counter was replaced by a recount (`epistemicCellsWithin`, :2931), so it cannot drift,
   but each committed ingestion pays an O(ceiling) C++ walk of the arena, which the codebase review
   should judge against the Vortex rule. (c) "A refusal writes nothing" compares only
   clause/support/record/status, not record contents, streams, dedup or counters, so a refusal that
   leaves a stream update or a dedup entry (a phantom duplicate on the next resend) passes; a
   `Duplicate` is never checked for unchanged health or contact. (d) At about 2 executions per second
   a campaign covers a few thousand inputs; the five corpus files are most of the evidence. The
   two defects the fuzzer found (non-finite tuple, ingress state of a stopped actor) have a unit
   test each but no corpus regression input; the stray `crash-deffbbef...` file in the repo root
   could be that input and should be committed into the corpus or deleted.
1. **Dead and unreachable code.** `ArenaExhausted` is classed in `outcomeClassOf` (:3007) and never
   produced; arena cell-allowance exhaustion would surface as `IngestionFault` (needs host action,
   not charged), which reads as a program defect. The pin branch in the history trim and the retire
   turn is unreachable until I3-4 and untested ("excision spares pinned records" is on the design's
   test list), so I3-4 must add the test with the first pin writer.

## Where the pipeline is sound

- Digest and dedup: origin-signed fields only, host-mapped times and `relayedBy` excluded; relayed
  and direct copies share a digest (mutant `digestHasRelay` is caught); a duplicate commits nothing
  and refreshes nothing (tests and the fuzzer's resend check).
- Counter policy: direct content refusals count and show contact; every other refusal and any
  relayed refusal is `refused_count` only; the gate's `reject_count` stays apart; the three host
  conditions are never charged. Mutants that swap or drop these die in unit tests.
- Advance and retire: older-incarnation records on every key become `retired` (the mutant that
  inverts the comparison dies in the unit, property and fuzz layers); dedup and streams reset;
  latches stay; the tombstone keeps the replay floor and takes no share of `max_sources`.
- Unscored defaults: `UNTESTED`, immutable verdict, and no calibration record cannot be read as
  endorsement; nothing in 1b/1c ever writes another verdict.
- Typed fields and units: sequence and incarnation are range-checked against Int64, times and
  variance finite and bounded, frame and units compared, both receipt stamps by the executor.

Tally: BLOCKING 0, MAJOR 4, MINOR 11.

Verdict: FAIL
