# I3-2 design review, round 2: sensor and hardware-fault expert (persona hardware)

Scope: `i3-2-design.md` revision 2 (commit `cae4a6ed`, unchanged at HEAD `3ae8d384`) and my round-1
report `review-hardware.md`, against P19 sections 2.1, 4, 5.1, 7 and 8.2, the I3-1 design
(`i3-design.md` sections 3 and 14) and the code at HEAD `3ae8d384`. Paths below are relative to
`apps/common/xanadu/vagent/runtime/` unless they start with `design/`. Simulations:
`/tmp/p19-hardware/sim.py` (run with `python3`; output quoted below, sections A to I).

## Earlier findings

| Round-1 finding                                      | Status in revision 2                                      |
| ---------------------------------------------------- | --------------------------------------------------------- |
| M1 SUSPECT unreachable for slow sources              | Resolved in kind; new defects in M1, M2 and m1 below      |
| M2 health actions mis-ordered against queued work    | Resolved                                                  |
| M3 isolate gated by exhausted budgets                | Partly resolved; see M6                                   |
| M4 reasons exhaust, cost half the budget             | Resolved; m3 remains                                      |
| M5 grant check authenticates nothing                 | Resolved; see M6 for what the token does not cover        |
| M6 backlog and clock-fault bursts charged as content | Resolved by exclusion; the exclusion now hides faults, M3 |
| M7 `-pred` wedge hits reset                          | Fixed for the wedge; the fix is unsound as written, M7    |
| Minors                                               | Nine resolved, one not resolved (reflash runbook, M4)     |

**M1 (rate-aware evidence): resolved in kind.** An event-indexed fraction removes the sample-rate
dependence I measured: the old count needed a period under 35 s to reach 3 and tripped a 100 Hz
source at 1 in 2000. The fraction has no clock, so the stale-flush, stall-overweight and `h` domain
findings (M6, minors) disappear. Output A shows the price: detection and recovery now scale with the
source's period (hourly: 14 h to SUSPECT from clean when every reading is refused, 64 h back to
NOMINAL). That is acceptable for display but see m2.

**M2 (ordering): resolved.** Section 7.3 names one control predicate for the four places that test
`sourceChange.has_value()` (`agent_executor.cpp:592-598`, `:3758-3763`, `:3695`, `:3702`) and adds a
reset epoch that supersedes queued readings and drops coalesced charges. The epoch is the fix I
asked for. One edge in m5.

**M3 (isolate under exhaustion and flood): partly resolved.** The exemption from `EpistemicBudget`
and `ArenaExhausted` (7.4) and the ingress mirror answer what I asked. Three consequences are not
handled: the mirror takes effect only after the turn commits, exempt actions are unbounded in slots
when they change nothing, and isolating the flooder does not restore service to a full arena. See
M6.

**M4 (reasons exhaust): resolved.** Reasons are unique by (kind, code), bounded at 4 per profile,
priced in section 9, and `Clear` removes them without a reset. Residual semantics in m3.

**M5 (forgeable grant): resolved.** A 128-bit host-side token authorizes; the ID is a label; the
executor stamps the wall time; `actions` records what was done. The token and the audit trail are
only as durable as the host process, see M6.

**M6 (bursts charged as content): resolved by exclusion, with a cost.** Stale and time-mismatch
refusals feed nothing, so a link-outage flush cannot move a healthy source. The same exclusion makes
a real device clock fault invisible; M3 asks for the missing diagnostic.

**M7 (wedge blocks reset): fixed, but not as written.** Settle after commit does let a reset commit
under a failing handler. The settle turn itself is all-or-nothing and discards events; M7.

**Minors from round 1.**

- Hysteresis: the ML gate's reasoning (state is display only; I4 uses `f`) is accepted. Output C and
  the 100 Hz scaling are noted in m6.
- `h` domain, steady-clock suspend, stamp semantics, sentinel `atWall`, tombstones, outcome map,
  grant lifecycle and bounds: resolved by section 3.2 (no clock), 4, 7.1 and 7.2.
- **Repaired device restart: not resolved.** The runbook I proposed (reset, then rebind at a higher
  incarnation) cannot work for the devices it is for. I withdraw it; M4 gives the reason.
- Answers to the section 10 questions of revision 1: `Clear` was adopted. `reinstate` (clear the
  latch while keeping readings) was not, because P19 5.1 says only reset unlatches. That is the
  owner's to confirm, see Questions.

## What is sound

- The event-indexed estimator is bounded by construction (`0 <= R <= N <= 1/(1-b)`), needs no clock
  and no rebase, and cannot go non-finite. I re-derived the closed form for a burst from clean
  steady state: `f = 1 - b^B`, so 14 consecutive refusals trip SUSPECT at H = 32 (output B). A 100
  Hz EMI glitch of 10 events does not; 15 events do. Rate does not matter, only count.
- A silent source keeps its fraction. That matches P19 5.1 (silence establishes missing data, not
  cause), and keeps a repaired-but-quiet device from healing by waiting.
- Excluding `ClockUnsynchronised`, `IngressBackpressure`, `UnknownSource`, `IngestionFault`,
  `TurnExhausted`, `ArenaExhausted`, `EpistemicBudget`, `UnboundIncarnation` and `Undelivered` from
  health is right: each is the host's or the program's condition (`agent_executor.cpp:900-909`).
- Settle-after-commit as the repair is the right shape: the operator's state change must not depend
  on the program's handlers.
- Issuer never a bound or named source (7.1) closes the self-clear path P19 4 requires.
- Reset keeps dedup and streams (P19 2.1), and `SupersededByReset` fixes the case where a reading
  received before the reset becomes the "new eligible observation".
- Exempting isolate, clear and reset from the ceiling follows the source-change precedent
  (`agent_executor.cpp:1862-1868`).

## BLOCKING

None.

## MAJOR

### M1. Relayed readings count in N but their refusals do not, so gateway fleets read NOMINAL

(design 3.1, 3.2; `agent_executor.cpp:878-889`, `epistemic_ingress.hpp:27-29`)

Problem: section 3.1 charges health only for "a directly received event" and 3.2 increments N for
"an accepted reading, direct or relayed". The two populations differ.
`CheckedObservation::relayedBy` is set "whenever [the transport principal] differs from the origin
signer", so every sensor behind a gateway, hub, LoRa concentrator or mesh node is relayed on every
event. Its accepted readings raise N; its refused readings go to `refused_count` (`:885-887`) and
never to R.

Evidence (output D, H = 32, threshold 0.25): a source with 30% bad readings reads SUSPECT 84% of the
time when direct, 3% when half its traffic is relayed, and never when 90% or all is relayed. A
source that is 100% bad and fully relayed never reads SUSPECT.

Consequence: in the most common field topology, the estimator that 2a ships is blind, and the fleet
reads NOMINAL while every reading is refused. The relay rule exists so a relay "cannot make a victim
SUSPECT". For the three refusals whose fields are in the signed digest (frame, units, variance;
`agent_executor.cpp:3668-3676`), a relay cannot cause the refusal: a changed field fails signature
verification in the adapter before `observe`. Only replay of genuine old bad events is possible, and
`firstCharge` already counts each event ID once.

Required fix: make R and N count the same population, and treat a relay the host trusts as direct.
Concretely, (a) count relayed refusals of signed-content kinds in R when the relay is one the
binding declares (`EpistemicSourceGrant` gains an optional list of first-hop relay principals), and
(b) where no relay is declared, count a relayed accepted reading in neither R nor N, so the fraction
at least measures one population. Add tests at 0%, 50% and 100% relayed for a 30% source, and the
replay case (a relay replays 14 old distinct bad events; state the bound that results).

### M2. Under load the estimator counts every refusal but only the readings the queue lets through

(design 3.2; `agent_executor.cpp:3628-3657`, `:3745-3770`, `:1976-1980`)

Problem: every health refusal is decided in `observe()` before the queue check, and is charged at
once, with no cap on how many are charged per wake. Accepted readings go through the per-source
queue (8 slots by default) and `ingestPerWake = 4`. A reading that passes the content checks and is
then dropped for `IngressBackpressure` is uncharged and uncounted (`chargeable`, `:900-909`). Under
overload the numerator therefore sees all the bad events and the denominator sees only the
throughput. The fraction measures `bad offered / (bad offered + good served)`.

Evidence (output C, Poisson arrivals, 4 ingestions per wake, per-source share 8, 600 s):

| True bad rate | Offered | Actor capacity | Fraction of time SUSPECT, as designed |
| ------------- | ------- | -------------- | ------------------------------------- |
| 5%            | 100/s   | 20/s           | 13%                                   |
| 5%            | 1000/s  | 200/s          | 13%                                   |
| 5%            | 1000/s  | 50/s           | 100%                                  |
| 10%           | 1000/s  | 200/s          | 99%                                   |
| 10%           | 100/s   | 20/s           | 99%                                   |

The design's own table says 5% and 10% sources stay NOMINAL. Ten 100 Hz sensors on one actor that
serves 200 ingestions/s each get 20/s. The batch order inside a wake does not rescue it (output C,
"batch first"). Counting the dropped-but-content-clean readings in N removes the effect (same rows:
0.0%, 0.0%, 0.1%, 0.0%, 0.1%).

Consequence: a healthy source on a loaded actor reads SUSPECT, which I4 will convert into inflation
or ineligibility, and an incident that floods an actor makes every source look faulty at the moment
the operator needs the display to discriminate.

Required fix: when `observe()` reaches the `IngressBackpressure` refusal (`:3770`) after all content
checks passed, record a "passed" charge in `Ingress::Charge` (a third counter) and apply it as an
accepted event in the next bookkeeping turn (`N <- b N + 1`, `R <- b R`). Add a test with a stalled
pump: 1000 offered, 5% bad, queue full, the source must stay NOMINAL. The tests in section 9 sweep
sample rates but not actor load.

### M3. Section 3.1 classifies by code name, not by cause, and hides real device faults

(design 3.1; `agent_executor.cpp:3622-3657`, `:895-899`; `compiler.cpp:5362-5376`)

Problem: four separate defects, each a refusal whose cause the design reads wrongly.

1. **A device clock fault is invisible.** A battery-less device whose RTC resets to 1970 after a
   brownout sends every reading with a stale `acquiredAt`. A device whose clock runs ahead sends
   future times. Both are refused (`:3646-3648`) and, by 3.1, move neither R nor N (output E: after
   10,000 stale refusals R = N = 0, NOMINAL). Those checks also run before the frame and variance
   checks, so a device with a bad clock hides its content faults as well. P19 section 7's table
   lists time and sequence diagnostics, and explicit stale state, as the mitigation for timing
   faults; the design leaves only the lifetime `content_refusals` total. The design's reason (a host
   fault would move every source) is a correlation argument, and it can be tested: a host fault is
   shared, a device fault is not.
1. **`UncertainAcquisition` is treated as the device's claim.** Section 3.1 says "the device itself
   claims it cannot place the reading in time". P19 3.2
   (`design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:338`) defines
   `acquiredUncertainty` as the clock-mapping uncertainty bound, and `CheckedObservation.acquiredAt`
   is "mapped seconds". The mapping is the host's (I3-3 builds it), and its bound grows with the age
   of the last clock sync. A host mapping outage therefore turns every source SUSPECT through this
   refusal, the common-cause case 3.1 excludes for the other two time refusals.
1. **`InvalidObservation` mixes a dead sensor with an adapter defect.** The code covers a non-finite
   or mistyped reading (`:3629-3631`, `fitsKind` rejects NaN and infinity) and an empty event ID or
   an out-of-range integer (`:3628`). A sensor emitting NaN or a wrong-typed tuple is the textbook
   failed ADC or firmware fault, and it is the clearest device evidence there is, yet it stays
   diagnostic while an out-of-range variance counts. `chargeable` takes a code only, so the split
   needs the check site to name the cause.
1. **`StaleSequence` after a reboot looks like a replay.** A device that lost its counter restarts
   at sequence 1 and is refused `StaleSequence` for every reading (`compiler.cpp:5362-5366`),
   charged to `refused_count` (feeds nothing). It reads NOMINAL with 100% of readings refused.

The arena also raises `ObservationTimeMismatch` for a regressed clock epoch (device) and a regressed
mapping version (host) under one code (`compiler.cpp:5368-5376`).

Consequence: the displayed state says NOMINAL for the commonest field failures: lost RTC, lost
counter, NaN output. The estimator works; the evidence it is fed is wrong.

Required fix:

- Keep time-class refusals out of the SUSPECT fraction, but give them their own decayed fraction on
  the profile (two more Double fields, same update) and surface it as a distinct diagnostic reason
  code (`clock_fault`) in `rtHealth`'s output. Add a per-actor correlation test in the executor:
  when at least half of the bound sources that sent within the last window have a time-class
  fraction over threshold, report `common_cause` instead of a per-source clock fault. This is the
  part I3-3 inherits (clock mapping).
- Move `UncertainAcquisition` into the same class as the time refusals, or justify in the design why
  the uncertainty is the device's. Split epoch regress (device) from mapping-version regress (host)
  by distinct turn results.
- Split `InvalidObservation` at the check site: non-finite or mistyped tuple values and oversized
  signed bytes are health refusals; empty ID, out-of-range integers and unequal arity are adapter
  defects.
- Count a run of `StaleSequence` refusals with distinct, increasing-from-low event sequences as a
  `sequence_regression` diagnostic (not a health refusal, because a replaying attacker produces the
  same code); the runbook in M4 starts from it.
- Tests: RTC-reset device, NaN device, counter-loss device, and a host mapping outage across 10
  sources (no source becomes SUSPECT; `common_cause` is reported).

### M4. The reflash runbook cannot work for a device with no persistent boot counter

(design 7.4 last paragraph, 12)

Problem: the design says the runbook for a reflashed device is "reset, then rebind at a higher
incarnation". The incarnation is a signed field of the device's own event (`kObsIncarnation` is in
`signedFields`, `agent_executor.cpp:3668-3676`), and `observe` requires events to match the
binding's incarnation exactly (`:3617-3620`). A device that has no persistent counter cannot sign a
higher incarnation, so binding a higher one refuses all of its readings `StaleIncarnation`, and
binding the old one leaves its restarted sequences refused `StaleSequence`. This was my own round-1
runbook; it fails for exactly the devices it was written for.

Evidence: `i3-design.md` section 3 says "the device or its host identity layer owns that boot
counter". The device with no flash counter has neither, and Reset keeps the streams by design (P19
2.1), so Reset does not help either.

Consequence: a brownout-rebooted cheap sensor is permanently refused with no operator action that
fixes it, and nothing in the health state shows it (M3 point 4). An operator following the written
runbook will make it worse.

Required fix: delete the sentence and replace it with the real constraint. A source must obtain its
incarnation from something durable or fresh: a monotone persistent counter, a trusted RTC reading at
boot, or a challenge-response with the host at boot (the host supplies a counter inside a signed
handshake the device includes in its events). State that a device that can do none of these cannot
be admitted as an arbitrated source, and say how I3-3 exposes it (the `sequence_regression`
diagnostic of M3). If the owner wants an operator escape hatch, the only sound one is a separate
health action that clears one principal's stream floors and dedup under an explicit grant bit, with
its replay window stated (bounded by `max_age`); it would amend P19 2.1, so it is a question for the
owner, not a decision for this design.

### M5. Isolation is as durable as the last checkpoint, and the design says a reboot cannot shed it

(design 1, 7.1, 7.4; `agent_executor.cpp:4932-4960`)

Problem: section 1 claims health "that a reboot cannot shed". The latch, reasons and `actions` live
in the arena, which persists only through a checkpoint, and nothing in the executor takes one
automatically. The grants, the token and the ingress mirror are host memory and are lost on any host
restart. A host that restarts from a checkpoint older than an isolate (the usual crash case, and the
case in an incident) resumes the faulty source with no latch, no mirror and no audit record of the
isolate. The source rebinds, the host's binding table is rebuilt from configuration, and the device
the operator isolated 30 seconds ago is trusted again.

Consequence: the safety action is silently revocable by a crash. The audit list that Reset cannot
clear is also lost with the arena.

Required fix:

- Say precisely which reboots the latch survives: a device reboot (any case), an executor checkpoint
  and resume (yes), a host crash (only if a checkpoint followed the action).
- `HealthReceipt`/`healthOutcome` reports whether the effect is in a checkpoint taken since; the
  executor offers `checkpointAfterHealthAction` or documents that the host must checkpoint before it
  reports an isolate done.
- Make actions idempotent and report `NoChange` (already isolated, nothing to clear, reset of a
  clean profile) without a turn or records. The host can then keep its own durable isolate list and
  replay it at resume, which is the only defence against a lost checkpoint.
- Apply the isolate to the ingress mirror at call time, not after commit, and remove it if the
  action is refused. With the control item at the head of the queue the delay is usually one turn,
  but an actor in a long handler turn or a stalled shared pump leaves the flood running; the call is
  the operator's only certain moment.
- State that grant tokens do not survive a host restart; an operator tool must re-obtain them.

### M6. Exempt actions are not bounded, and isolating the flooder does not restore service

(design 7.4 "Budgets", 9; `agent_executor.cpp:1862-1880`)

Problem: isolate, clear and reset are exempt from `EpistemicBudget` and `ArenaExhausted`. The
justification is that they remove or hold cells. But arena slots are never reused (`:1871` comment),
the `actions` list evicts logically and frees no slot, and every accepted action writes about 16
action cells. Nothing keeps a second isolate of an isolated source, a clear with nothing to clear or
a reset of a clean profile from running a turn that allocates. An automation that retries every
second after a lost receipt allocates without limit through the only path that ignores both
ceilings.

Evidence: output H, 250 slots per ingestion: 2^30 slots last 11.9 h at 100 Hz, 119 h at 10 Hz, and
the "50 days at 1 Hz" in `epistemic_limits.hpp:40` is 1,193 h = 49.7 days. A flood-driven exhaustion
therefore falls inside one shift for a fast source.

Consequence: (a) a runaway operator tool, not only an attacker, exhausts the slots; (b) the design
never tells the operator that isolating the flooding source does not free the arena: healthy sources
keep refusing `ArenaExhausted` (`NeedsHostAction`) until a checkpoint and resume.

Required fix: no-op actions allocate nothing (`NoChange`, M5); a bound on exempt actions per
principal per window (or, simpler, charge the exempt turn's slots against a reserve between the soft
`arenaSlots` ceiling and the ephemeral bit and refuse past a hard second ceiling). Add the runbook
for exhaustion: isolate the source, checkpoint, resume, rebind, reset only after repair. Test: 1,000
repeated isolates of one source add a bounded number of slots.

### M7. The settle turn is all-or-nothing: one failing handler discards all other events

(design 6; `compiler.cpp:5679-5731`, `agent_executor.cpp:4932-4945`)

Problem: `rtSettle_` detaches the whole of `d.pending`, runs every event's handlers in one turn, and
`#Fail` undoes the turn. Design section 6 then runs `discardPendingEntry`, which detaches
`d.pending`. If an isolate drops supports for three predicates and one `-pred` handler fails, the
undo restores `d.pending` and the discard removes all of it: the handlers of the other predicates,
which would have succeeded, never run. Section 6's claim "a correct handler still sees every event
once" is false for any program that has both. The same holds when cycles or fuel run out on a source
with many supports (a large reset), where all events are discarded because the last one did not fit.

Consequence: an actuator-stop or alarm handler for a healthy predicate is skipped silently because a
different handler is brittle against the faulty source's garbage. The outcome `SettleFailed` tells
the operator that something failed, not what was lost. The discarded events are unrecoverable, so
fixing the handler later does not replay them.

Two smaller defects of the same design:

- Between the committing turn and the settle turn the arena holds events on `d.pending`. The design
  runs the settle turn inline "in the same wake", which also keeps `checkpoint()` refused for the
  window only if it is not enqueued as a host event. State that it is inline, and that `orderlyStop`
  and `dropHostEvents` (`:2013`, `:1967`) cannot interleave.
- Ingestion keeps the wedge: a reading that makes a `+pred` handler fail is refused `IngestionFault`
  (`NeedsHostAction`), never charged, so a source whose garbage crashes the program shows NOMINAL
  while the program refuses all its readings. Out of scope to fix, but the design should say so.

Required fix: settle one event (or one bounded chunk) per turn. A failing event is moved to a
bounded dead-letter list on `$agent` (a dimension, not a discard) with its predicate and error, the
remaining events continue, and the outcome counts `settled`, `dead_lettered`. A host action
re-drives the dead letters after the program is fixed. Replace the test line with: two predicates,
one handler failing, the other must run once per event.

### M8. The operator cannot see the state they are told to act on

(design 4, 5, 7.2; `agent_executor.hpp:361-380`)

Problem: the design adds a derived health state, a fraction, reasons, a fault record and an audit
list, and four actions. The executor's public surface offers no read of any of it (`ingestOutcome`,
`sourceChangeOutcome` and `epistemicCells` are the only accessors). Queries that read health are I4.
An operator in an incident therefore cannot tell which source is SUSPECT, which reason placed a
DEGRADED, whether an isolate took effect, how many events an isolated device is still sending, or
what the last 8 actions were. The receipt's outcome log is bounded at 64 per actor
(`epistemic.outcomeLog`), so after an incident that isolates 100 sources the early outcomes are
evicted, and "unknown ticket" is indistinguishable from "pending".

Fleet incidents have a second shape the API does not serve: no wildcard source and a pending cap of
16 per actor (`kMaxPendingHealthActions`) mean that isolating a bad firmware rollout of 100 devices
returns `IngressBackpressure` from the 17th call, and the caller must pace its own retries.

Consequence: the workflow "isolate, degrade, clear, reset" in the task is not usable without a
separate, out-of-band way to look at arena cells. P19 5.1 speaks of a displayed state, which implies
a reader.

Required fix: add to slice 2a or 2c a read accessor
`sourceHealth(pid, principal) -> optional<SourceHealth>` (state and deciding code, `f`, N, latch,
fault, reasons, the `actions` list, lifetime counters, last refusal, time since last receive, and
the host-side count of readings refused by the mirror), and `sourceHealthList(pid)` for the actor.
Distinguish `pending` from `evicted` in `healthOutcome`. Raise or exempt the pending cap for Isolate
(a batch call `healthAction(token, vector<principal>, Isolate)` that costs one slot and is applied
in order) and test a 100-source isolate.

## MINOR

- **m1. Minimum evidence is too small, and the text understates it (3.3, section 4).** The design
  says a fresh profile reads SUSPECT after three readings "if all three are refused". One refusal in
  three gives `f` of 0.33 or more, so one bad reading in the first three is enough. Output I: a 5%
  source reads SUSPECT within its first 40 events 13% of the time at `min_events = 3`, 6.5% at 5,
  2.3% at 8; a 10% source 32%, 23%, 15%. Every Reset and every new binding starts at N = 0. Set the
  default to 8 and say "two refusals", or keep 3 and correct the sentence.
- **m2. A repaired slow source stays SUSPECT for days, and only a destructive reset shortens it
  (3.3, 7.4).** Output A: hourly sensors need 64 h of good readings; 15-minute sensors 16 h. Reset
  is the only override and, from I4, discards calibration. Ask the owner (Questions) whether a
  `Clear` that also zeroes R and N is wanted, or state the delay in the schema notes.
- **m3. `Clear` with no code removes every `degraded` reason (7.4).** Section 11 quotes the spec as
  "reasons an operator placed", but I3-3 adds sensor-reported `degraded` reasons (battery,
  saturation) to this list, and P19 8.2 requires persistent degraded reasons to obey the health
  contract. Record the origin (`operator` or `sensor`) in each reason and make a blanket clear touch
  operator-placed ones only; a sensor reason needs its code named.
- **m4. A refused Reset still bumps the epoch and drops charges (7.3).** The epoch is bumped at call
  time under the mutex; if the queued action is then refused at turn time
  (`UnauthorizedHealthAction`, token revoked while queued) the readings queued before it were
  refused `SupersededByReset` and the charge entry was dropped, and no reset happened. Bump at turn
  time, or undo on refusal.
- **m5. Audit order depends on a clock that may be absent (5, 7.2).** Wall time is recorded only
  when synchronised, and incidents often coincide with time-service loss. Give each `actions` record
  a per-profile monotone sequence number so eviction and order stay readable. Do not log state
  transitions: the design's own table has p = 0.2, H = 32 changing state 132 times an hour at 1 Hz,
  so about 13,200 times an hour at 100 Hz.
- **m6. `HealthGrantToken` default-constructs (7.2).** A zero-initialised token in a request must
  never authorize. Make the type non-default-constructible or have `grantHealth` never mint zero,
  compare tokens in constant time, and count refusals per token.
- **m7. Tuning leaves N above its stated bound (3.2, 9).** The bound `N <= 1/(1-b)` holds for the
  settings in force. Changing H from 64 to 8 leaves N = 92.8 against a new bound of 12.0 until about
  24 events pass (output F). `R <= N` and `f` stay valid, so nothing breaks, but the invariant in
  the fuzzer and the "bounds after 10^6 updates" test must be written against the old bound or after
  a settle period.
- **m8. Reset epoch and mirror lifetimes (7.3, 7.4).** The mirror is rebuilt only when a reading
  meets the arena's refusal. After resume, an isolated source's first reading costs an ingestion
  turn. Rebuild the mirror from profiles at resume (a walk over at most `maxSources`).
- **m9. A latched tombstone holds budget for life.** Retire leaves about 10 cells and Reset on a
  tombstone is allowed; a site that replaces devices with new keys (secure-element identities are
  per device) keeps one tombstone per dead device. State the growth rate and that a new key bypasses
  a latch by design, so the latch protects the identity, not the slot; the operator must retire the
  old principal.

## Questions for the owner

1. **Operator escape for devices without a boot counter (M4).** Is a grant-gated health action that
   clears one principal's stream floors and dedup (amending P19 2.1) acceptable, or must such
   devices be declared unsupported?
1. **`Clear` of the suspicion fraction (m2).** Reset is the only way to make a repaired hourly
   sensor read NOMINAL before 64 hours. Do you want a non-destructive action that forgives R and N
   (not the latch), or is waiting acceptable?
1. **Mistaken isolate.** `reinstate` (clear the latch, keep readings and calibration) was not
   adopted because P19 5.1 allows only reset to unlatch. Confirm that a mistaken isolate of a
   calibrated source is meant to cost its calibration in I4.

## Fit to later iterations

**I3-3 (heartbeats, clock mapping, sensor health reports).**

- Silence timers must exempt latched sources, or the mirror's refusals must refresh `last_contact`;
  otherwise an isolated, still-talking device is reported silent, and a reset starts with an expired
  heartbeat. Record the rule and the mirror counter in M8.
- The clock-mapping work turns `acquiredUncertainty` and the mapping version into host products. M3
  must be settled first, or I3-3 will find `UncertainAcquisition` and mapping regress charged
  against devices. Record the interim classification and that I3-3 owns the split.
- Sensor-originated reasons go into `reasons` with an origin tag (m3), and "its recovery percept
  clears only reasons that source added" needs that tag.
- A gateway topology (M1) is the common case for health reports too; the relay declaration belongs
  on the binding and should be designed once for both.

**I4 (queries, scoring, gates, directives).**

- The continuous `f` at small N is noise (output I); I4 should use a shrunk estimate
  `(R + a)/(N + a + c)` or apply the penalty only above `min_events`, and must recompute state at
  query time as section 4 says.
- Use `f` only after M1 and M2 are fixed; otherwise it encodes topology and load.
- The directives lower onto `healthEntry`; the same idempotence and `NoChange` rules (M5, M6) apply.
- Calibration reset at Reset (P19 5.1) lands with a destructive cost that makes the missing
  non-destructive options (Questions 2 and 3) more expensive.

**Live-upgrade translator.**

- New profile fields (`health_refused`, `health_events`, `fault`, `reasons`, `actions`) and the
  three new entries must be in the translator's schema and `program.cpp` remap; a missed remap is a
  silent no-op (section 9 says so, and a test should cover it).
- R and N were accumulated under the settings in force; an upgrade that changes H does not rebase
  them (m7). That is sound but should be recorded.
- A pending dead-letter list (M7) must survive or be rejected at upgrade; a checkpoint with a
  non-empty dead-letter list should be allowed, one with unsettled `d.pending` must not.

Tally: 0 BLOCKING, 8 MAJOR, 9 MINOR.

Verdict: FAIL
