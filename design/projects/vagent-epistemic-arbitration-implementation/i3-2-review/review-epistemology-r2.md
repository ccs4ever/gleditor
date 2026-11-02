# I3-2 design review, round 2: epistemologist (persona epistemology)

Scope: `i3-2-design.md` revision 2 (commit `cae4a6ed`, unchanged at HEAD), against P19 sections 2.1
(preservation, lines 228-242), 4 (`EpistemicHealthGrant`, 472-476) and 5.1 (latch, reasons,
displayed state, 558-585), the owner's Q1-Q3 rulings and the I3-2 additions (`clear`, the wedge
fix), and code at HEAD `3ae8d384`. Persona: what the system claims to know, and on whose authority.
Scratch computation: `/tmp/p19-epistemology/r2.py`.

## Earlier findings

| Round 1                               | Status                       | Why                                                                                                                                                                                                                                                                                                                              |
| ------------------------------------- | ---------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1 reset undone by earlier readings   | resolved, with residuals     | The reset epoch (7.3) makes readings queued before a reset refuse `SupersededByReset`, and the epoch tag also covers refusal notes. The residuals are m6 here and M1 below: the epoch is host-only, bumped at call time, and leaves no record in the arena.                                                                      |
| M2 staleness scored as source quality | resolved                     | 3.1 limits health refusals to four content codes. `StaleObservation` and `ObservationTimeMismatch` stay in the lifetime total only. The 1000-reading stale flush test covers the common cause. The code at HEAD still lists both in `isContentRefusal` (`agent_executor.cpp:895`), so the refusal note needs a third count (m9). |
| M3 no durable account of who did what | partly resolved              | `actions`, incarnations on records and unique reasons are in. The list is evictable by the lowest-privilege action, and the reset entry drops the latch provenance (M2 below).                                                                                                                                                   |
| M4 FAULTED changes no answer          | resolved                     | Option (a) is adopted (7.4): isolate drops supports and ingestion refuses. The scope of what that licenses is in "Isolate" below.                                                                                                                                                                                                |
| M5 isolate refusable under pressure   | partly resolved              | The ceiling, `max_sources` and the cell `admit` no longer gate the safety actions. The arena slot gate is waived without a reserve, which is the same failure moved (M4 below).                                                                                                                                                  |
| M6 decay across a checkpoint          | resolved by redesign         | The estimator is indexed by events and uses no clock, so there is nothing to rebase. A silent source keeps its fraction, which matches P19 5.1 ("silence establishes missing data, not its cause").                                                                                                                              |
| m1 self-issued actions                | partly resolved              | The rule is stated and canonical (principals are lowercase 64-hex, `agent_executor.cpp:3361`, the same form as `issuer`), but it is checked only when the grant is installed (M3 below).                                                                                                                                         |
| m2 grant checked once                 | resolved                     | Rechecked at turn time (7.2). The grant ID is written to the arena `actions` entry.                                                                                                                                                                                                                                              |
| m3 unbound principal                  | resolved                     | `NoProfile`, `NeedsHostAction`, "bind it first" (7.4). A recorded limit.                                                                                                                                                                                                                                                         |
| m4 reason identity                    | resolved                     | Unique by (kind, code). A repeated degrade by a second operator keeps only the first provenance, so the second appears only in `actions` (see M2).                                                                                                                                                                               |
| m5 derived SUSPECT has no reason      | resolved                     | `rtHealth` returns the deciding code (4). See m8 on namespace.                                                                                                                                                                                                                                                                   |
| m6 stamp versus live state            | resolved                     | Section 4 states the two-consumer rule, including "never read the stamp for eligibility".                                                                                                                                                                                                                                        |
| m7 `rejection_streak` not read        | resolved                     | Stated in 3.3 and section 10.                                                                                                                                                                                                                                                                                                    |
| m8 settings outside provenance        | resolved in principle        | 3.4 says the stamp is the state under the settings in force at ingest. "The turn record carries the values used" names no record that exists (m4).                                                                                                                                                                               |
| m9 reboot cases                       | resolved                     | The 2c test row lists a reboot (advance), retire and revive, and a checkpoint, each keeping latch, reasons and actions.                                                                                                                                                                                                          |
| m10 refusal-only profile              | text resolved, tests missing | Section 4 states the rule. No listed test covers a refusal-only profile reading SUSPECT, or its reset (m11).                                                                                                                                                                                                                     |

## What is sound

- **The estimator's claim is honest about what it measures.** 3.3 declares SUSPECT to mean "about a
  quarter or more of the last few dozen outcomes were unusable content". That is a statement about
  message production, not about the accuracy of accepted readings, and it names its sample basis as
  P19 5.1 requires. I recomputed the figures: the bound is 1/(1-b) = 46.67 at H = 32; 14 refusals
  from a long clean history reach SUSPECT; 64 good readings return a saturated source below 0.25.
- **Latch semantics match 5.1.** Advance keeps latch, reasons and actions. Only reset unlatches.
  `clear` cannot touch the latch or `suspect` reasons. A derived fraction cannot be cleared by an
  operator, only by evidence, or by a reset that is itself recorded.
- **Unhealed evidence is not manufactured.** Relayed refusals cannot make a victim SUSPECT, and
  refusals are charged only for checked principals.
- **Two-consumer rule.** Stamp for audit and arrival view, query-time recompute for eligibility and
  scoring, inflation once, calibration trained whatever the stamp. This is the right split.
- **Fail-closed on a failing handler.** The state change stands and the events are discarded. For a
  safety action the operator's authority should win over a program's handler.
- **The settle-after-commit ordering is argued correctly**: running the settle turn before any other
  item stops leftover events from failing an unrelated ingestion.

## BLOCKING

None.

## MAJOR

### M1. Reset and isolate leave no durable arena marker on the records they invalidate

Problem. Section 7.3 invalidates pre-reset readings with a host-side epoch that is not checkpointed
and never reaches the arena. In the arena, reset and isolate drop supports through the advance's
drop body, which marks each record `status = retired` (`compiler.cpp:5015` for the advance, `5108`
for retire) and moves it to the retired chain. A record retired by a newer reading, by an old boot,
by a retire, by a fault isolate and by a reset carry the same `retired` status. The calibration
epoch that the record carries is the constant 0 (`agent_executor.cpp:3725`), and the design's reset
does not change the incarnation.

Evidence. P19 2.1 (lines 156-160): "resetting a source ... creates a new epoch/incarnation binding
and expires old pending labels/completions before they can train it", and a late label must match
the full key including "calibration/model epochs" or return `ExpiredLabel`. After a reset the key of
a pre-reset record (incarnation, domain, epoch 0) equals the key of the post-reset live learner
exactly. 7.4 defers calibration to I4 "in this same turn", but I4 cannot recover a distinction that
I3-2 never wrote: records retired by a reset hold nothing that says so.

Consequence. A pending verification label for a pre-reset reading trains the freshly reset learner.
That is the exact outcome 2.1 forbids. The same absence blocks any audit question of the form "why
did this belief disappear": retired-by-supersession (benign), retired-by-fault (adverse) and
retired-by-reset (administrative) look alike. A calibration pass cannot decide whether to treat a
retired record as clean evidence about the claim (supersession) or as evidence discarded for a cause
that says nothing about the claim (fault, reset).

Required fix. In 2c, add to the profile an Int `reset_epoch`, incremented by `Reset`, and have
ingestion stamp it on each record (a new observation field beside `calibration_epoch`, or make the
stamped `calibration_epoch` read the profile's value instead of the constant). Add a `retired_by`
text field on a record, set by the drop body from its mode (`superseded`, `advance`, `retire`,
`isolate`, `reset`), so a record dropped by a fault is distinguishable from one superseded by a
newer reading. Pin both fields in the ABI test. Keep the host epoch for the queue. Test: a pre-reset
record is retired with `retired_by = reset` and the pre-reset epoch; a post-reset record carries the
new epoch.

### M2. The lowest-privilege action can erase the `actions` audit; a reset forgets who latched

Problem. Section 5: `actions` is bounded by `vagent.epistemic_health_actions` (default 8), "oldest
evicted", and a repeated `degrade` with an existing code "adds no reason ... and is recorded only in
`actions`". Reset records "the codes it removed", not the removed fault's issuer, grant, incarnation
or time, and `fault` is erased by the reset (7.4). The claim in section 5 that "the profile always
says that a fault existed, who latched it and who cleared it" holds only until the eviction.

Evidence (`/tmp/p19-epistemology/r2.py`). Sequence: `isolate` by operator A, `reset` by A, then
eight `degrade` calls with an existing code by operator B (a holder of a `Degrade` grant only).
Result: the eight entries are all B's; neither the isolate nor the reset remains. Seven repeats
evict the isolate and the eighth evicts the reset (checked by simulation for 1 to 9 repeats). A
`Clear` grant holder can do the same. No other record keeps the information: the fault is gone,
reasons are emptied, and "the host keeps its own log" (section 5) has no API, format or requirement
in the design (the receipt log is the per-actor outcome log, which is bounded and keyed to tickets).

Consequence. The record meant to show that a hardware fault was latched and cleared can be erased by
anyone holding a weaker grant, or by an automated operator that repeats itself. A flapping `degrade`
loop is also a plausible accident. The P19 2.1 requirement to preserve "fault latches,
warning/suspicion reasons and their provenance" is then true only until the next eight actions.

Required fix. (1) Evict by priority, not age: never evict an `isolate` or `reset` entry to admit a
`degrade` or `clear` entry; evict the oldest `degrade`/`clear` first, and bound latch entries
separately (say the last 4 isolate/reset pairs). (2) A repeat of an existing (kind, code, issuer)
increments a count on the existing entry instead of appending. (3) A reset entry copies the removed
fault's issuer, grant, incarnation and time, and the derived state and deciding code before the
reset, so the entry stands alone after `fault` is erased. (4) Keep a monotone `actions_evicted`
count on the profile so a reader can see that the list is a summary. (5) State, in section 5 and the
acceptance text, that the durable audit is the host's log, name its minimum content (grant ID,
issuer, principal, kind, code, outcome, times) and who writes it, or add it to 2c's API.

### M3. The self-certification ban is checked only when the grant is installed

Problem. Section 7.1 refuses a grant whose `issuer` equals a principal in `sources` or "any
principal bound to the actor" at the time of `grantHealth`. Section 10 then states: "The self-clear
rule holds from 2c: no sensor identity can hold a health grant (section 7.1)". Neither claim is
enforced later. `bindSource` (`agent_executor.cpp:3414`) checks aliases, incarnations and the
program, not live grants. The turn-time recheck in 7.2 is described as token revoked or expired
only.

Evidence. Sequence: operator identity K gets a `Reset` grant over source S1 (K is bound to nothing,
the grant is accepted); the host then binds K as a sensor on the same actor (accepted); K, as a
sensor identity, now holds health authority over its peer S1 and, if S1 is replaced by K in the
grant's list through a second grant, over itself. P19 4 (lines 472-476) says a sensor "cannot clear
its own latched fault without reset authority", and P19 8.2 requires "self-issued ... rejected".

Consequence. The ban holds on the ordering "grant first, bind later" only by luck of the host's call
order. Because binding and granting are separate host calls, the order is the host's, and an
operator who is also a sensor fleet member is precisely the case the rule exists for.

Required fix. Enforce at three points: `grantHealth` (as now), `bindSource` (refuse a principal that
is the issuer of a live health grant on any actor of this executor, `InvalidPrincipal`, or revoke
the grant and record it), and the turn-time recheck (refuse if the issuer is now bound). Decide the
scope: host-wide (all actors' bindings are in `ingress_->bindings`) is the safer reading of "a
sensor identity can never hold health authority" and costs one map walk. Test both orders and an
issuer bound on a different actor of the executor.

### M4. The safety actions are waived from the arena slot gate with no reserve

Problem. Section 7.4: isolate, clear and reset are "exempt from the cell ceiling and from
`ArenaExhausted`", justified because each "removes or holds cells except for the bounded provenance
and action records". The gate at `agent_executor.cpp:1873-1878` is not a ceiling on live cells; it
stops all epistemic turns once the arena's slot count reaches `arenaSlots`, with the comment "Slots
are never reused ... no epistemic turn runs, rather than fail mid-turn". The allowance exists so
that a turn does not fail mid-way when the arena fills. Waiving it for the health actions lets them
run exactly in that regime.

Evidence. Slots are never reused, so evicting an action entry (M2) or excising a reason does not
return its slots. A health action allocates a `fault` record (about 14 cells), an action record
(about 16) and, for an isolate, a profile if absent. Each permanently consumes slots. The bound on
cells in 7.4 ("bounded by the action list") counts live cells, which do not measure slot use. A turn
that finds the arena physically full undoes itself, so the latch is not set: the exemption converts
the clean refusal (`ArenaExhausted`, host acts, checkpoint and resume) into the failure that the
allowance was built to avoid, and the safety action is the one affected.

Consequence. Under the flood the design is meant to survive, an isolate can still fail, and repeated
exempt actions can run the slot count past the allowance without limit.

Required fix. Reserve slots for health actions: ordinary turns refuse at `arenaSlots - reserve`,
health actions at `arenaSlots`, where `reserve` is a derived constant (`kMaxPendingHealthActions` x
the worst-case slots per action, set in `epistemic_limits.hpp`, validated against `arenaSlots`).
Past `arenaSlots` even health actions refuse `ArenaExhausted`, with the refusal in the outcome log
and a log message. State the slot arithmetic in section 9. (The hardware reviewer owns the sizing; I
record that the correctness claim in 7.4 is false as written.)

### M5. `clear` with no code removes reasons that no operator placed, against the amended spec text

Problem. Section 11 words the P19 amendment as: "`clear` removes degradation reasons **an operator
placed**". Section 7.4 defines `Clear {}` (no code) as removing "every `degraded` reason", and the
reason record (section 5) has grant and issuer but no origin. Section 10 says I3-3 will add
`degraded` reasons from authenticated sensor reports ("a configured warning").

Evidence. P19 5.1: a warning "is cleared by a recovery health percept clearing its corresponding
reason"; accepted measurements do not clear a battery or saturation warning. A device-reported
warning is evidence supplied by the source; an operator's `clear` with no code would remove it
without a trace beyond the code list in `actions`.

Consequence. When I3-3 lands, one operator action erases device-reported evidence, which the amended
spec says it does not. The record shape cannot enforce the narrower rule, and changing the shape in
I3-3 means translating stored profiles.

Required fix. Add `origin` to the reason record now (`operator`, later `device`, and `derived` is
not stored), set by the action that wrote it. `Clear {}` removes `degraded` reasons with
`origin = operator`; `Clear {code}` removes the named reason if its origin is operator and refuses
otherwise. If the owner wants a stronger operator override of device warnings, make it a separate,
explicitly granted action and record the removed reason's issuer in the `actions` entry. Test: a
reason with a non-operator origin (inserted by a test hook) survives `Clear {}`.

## MINOR

- **m1. What each state licenses is not stated.** The design defines the states and who computes
  them; it never says what a reader may infer. Record this (section 4):
  - NOMINAL: no recorded adverse health evidence of the kinds counted. It is not evidence that the
    source is live (I3-3) or accurate. A profile with N below `min_events` is NOMINAL with no
    evidence; it should say so.
  - DEGRADED: an authorized party recorded a reason. It licenses variance inflation of the source's
    accepted readings and says nothing about any claim's truth.
  - SUSPECT: licenses a penalty on the source's readings. SUSPECT from the fraction means unreliable
    message production and does not license a statement about the accuracy of readings it did
    deliver; SUSPECT from innovation rejections (I4) does. P19 gives both inflation 4. Have I4 key
    the inflation on the reason class that `rtHealth` returns, not on the state alone.
  - FAULTED: licenses excluding all the source's readings and withdrawing its supports. It does not
    license concluding that any past reading was false, and must not generate a refutation label.
- **m2. The fresh-profile claim is off by one.** 3.3 says a fresh profile reads SUSPECT after its
  first three readings if all three are refused. Computed: three refusals give N = 2.936, below
  `min_events` = 3, so the state is NOMINAL; four give N = 3.873 and SUSPECT. Either change the text
  to four, or compare against `min_events - 1`/lower the default, and make the min-events edge test
  assert it.
- **m3. Refusal classification for the two new codes.** `SourceFaulted` and `SupersededByReset` must
  be added to the exclusion list of `chargeable()` (`agent_executor.cpp:902`), or the host's own
  action charges `refused_count` and creates profiles for them. The latch check must come first in
  the ingestion routine, so a latched source's reading never reaches a content refusal that would
  charge it. A relayed copy of an isolated origin must refuse `SourceFaulted` and must not charge
  the relay; add the test.
- **m4. The stamp is not evidence.** A record carries the string `SUSPECT`, with no `f`, no `N`, and
  no settings, and 3.4 says "the turn record carries the values used", but no such record exists.
  The stamp is also not a function of the event log (coalesced charges, pump order), so two runs of
  the same inputs can stamp differently. Add the fraction at ingest (one Double) to the record, say
  in section 4 that stamps are not replay-deterministic, and drop the unreachable `FAULTED` stamp
  path (`compiler.cpp:5517-5521`) once ingestion refuses latched readings, or keep it and say why.
- **m5. Quarantined readings leave no trace.** A latched source's readings are refused at the
  ingress mirror, uncharged, with no counter. The operator deciding a reset has no evidence whether
  the device still talks or is sending usable data. Keep a host-side count and last contact time for
  refused-while-latched readings, readable through the existing accessors.
- **m6. Reset epoch edges.** (a) The epoch is bumped at the call (7.3) but a queued reset can still
  refuse at turn time (revoked, expired, `NoProfile`, `TurnExhausted`); the readings queued before
  it are then superseded with no reset having happened. Bump at commit, or record the case. (b) The
  refusal lambda overwrites a `Refused` outcome on resend (`agent_executor.cpp:3597-3606`), so a
  superseded event ID resent by the transport passes the new epoch and is ingested. Hold
  `SupersededByReset` against resends of that ID for the outcome-log window. (c) Dropping the
  coalesced charge entry loses lifetime diagnostic counts the design says stay; count them. (d)
  Reset invalidates by receipt time, not acquisition: a reading acquired before and delivered after
  the reset counts as new. Record the limit and have I3-3/I4 compare mapped acquisition with a reset
  time kept on the profile.
- **m7. Discarded settle events are anonymous.** `SettleFailed` counts discarded events but names
  none. For an isolate or reset the agent never sees the withdrawal of those beliefs. Record the
  predicates and tuples discarded (bounded) in the outcome, so the operator can re-drive the
  consequence.
- **m8. Reason codes share a namespace with derived codes.** An operator code `refusal_fraction` is
  indistinguishable from the derived reason. Have `rtHealth` return (origin, code), or refuse
  operator codes that collide with reserved ones. Also note for I3-3 that the reasons bound (4)
  refuses a new reason: an operator filling the list would block device-reported or gate-derived
  reasons. Reserve a slot per kind for evidence-driven reasons.
- **m9. The refusal note needs a third count.** The note carries `content` and `other`
  (`agent_executor.cpp:3929` onward). Splitting health refusals from the other content refusals that
  refresh contact needs a new field, `charge` classification and ABI constant. Say so in section 9.
- **m10. Estimator asymmetry for relayed readings.** Accepted relayed readings add to `N` (3.2:
  "direct or relayed"); relayed refusals add to nothing. A source whose bad frames arrive only by
  relay is diluted by the same source's good relayed readings and never accumulates `R`. Either
  count relayed accepts and relayed health refusals both (the origin signature binds the bytes), or
  neither.
- **m11. Tests to add.** A refusal-only profile that reads SUSPECT, and its reset; reset refused at
  turn time; audit eviction priority (M2); `Clear` against a non-operator origin (M5); a pre-reset
  record's `retired_by` and epoch (M1); a relayed copy of an isolated source; a fleet-wide
  frame-policy change (all sources SUSPECT at once) that heals after the policy is fixed.
- **m12. `UncertainAcquisition` as unusable content.** A GNSS receiver without a fix answering
  honestly that it cannot place the reading in time is behaving correctly, and is scored as
  unreliable. It self-heals in about 8 good readings, but it penalises honest abstention. Owner's
  call (question 3).

## Isolate dropping supports

A fault latch is an authority's statement about the source, not evidence about any claim. The
question is whether acting on it by withdrawing supports is justified. My assessment of revision 2:

- **Licensed as withdrawal of testimony.** A support is a source's testimony for a claim. When an
  authorized party discredits the source, withdrawing its testimony is what the latch means. A claim
  held only on that testimony stops being believed (not believed false); a claim held also by
  another source keeps its other support. That is the same operation Q1 approved for an advance and
  matches P19 5.1 ("Active fault status excludes all source readings").
- **Not licensed as evidence about the claim.** The drop must not refute, must not produce a
  verification label, must not move any calibration count, and must not set a record's verdict.
  `rtBDrop` is also the route of `forget` and refutation, so the isolate path must call it without
  the label step. State this in 7.5 and test it. The cause must be readable afterwards (M1,
  `retired_by`) so a `-pred` handler or auditor can tell loss of confidence from change in the
  world; consider adding the cause to the `-pred` event.
- **SUSPECT and DEGRADED do not drop supports, correctly.** Their basis (a fraction, an operator
  warning) would justify a penalty, not withdrawal. Keep it so.
- **The cost of a mistaken isolate is the whole current belief set from that source**, recoverable
  only by a reset and fresh readings. That is the spec's rule; record it in the runbook beside the
  reflash note.
- **Ingress refusal is justified** by 7.4's argument (the only exit invalidates old readings
  anyway), with the loss in m5.

## Questions for the owner

1. Should the self-certification ban be host-wide (any principal bound on any actor of this
   executor) or per actor? I recommend host-wide (M3).
1. Should readings refused from an isolated source be counted and their last contact kept, so an
   operator can judge a reset (m5)? I recommend yes, host-side only.
1. Should `UncertainAcquisition` count against a source's health (m12), or stay diagnostic with
   staleness until I3-3?
1. If you want an operator to override device-reported warnings in I3-3, should that be a separate
   explicitly granted action (M5)?

## Fit to later iterations

**I3-3 (heartbeats, clock mapping, sensor health reports).**

- Reason producers other than operators need `origin` and a reserved capacity in the reasons list
  (M5, m8). A recovery percept clears only reasons with `origin = device` and `issuer` equal to the
  reporting principal; it never clears a latch (`fault` remains reset-only).
- Heartbeat expiry and freeze flags are separate eligibility flags and must not write into `R`/`N`
  or the reasons that `rtHealth` reads (P19 5.1: they "affect eligibility separately from gate
  rejection counts"). I3-2 should record that `R`/`N` are content evidence only.
- The estimator uses no clock, so there is nothing for a clock mapping to rebase, and "unknown
  mapping never clears a latch" holds because nothing in 2c clears by time. An absent `last_receive`
  must be treated as "never supplied a reading", never time 0 (stated in section 4).
- Reclassifying staleness and time mismatch as health refusals needs a versioned rule: record the
  list of counted codes as an ABI constant so a stamp can be read against the rule in force.
- The wall-time-on-action rule needs a clock-synchronised flag per entry (present: absent when
  unsynchronised). Keep list order as the ordering authority.

**I4 (queries, scoring, gates, directives).**

- Eligibility reads the live state through `rtHealth`; the stamp is an audit view (stated). Apply
  inflation once; key it on the reason class (m1).
- Needed from I3-2 and not written: `reset_epoch` and `retired_by` (M1); a reset time on the profile
  for acquisition-time comparison (m6d); the isolate drop never produces a label (above).
- `ExpiredLabel` and calibration reset depend on M1. Without it I4 cannot implement P19 2.1's "reset
  expires pending labels".
- Language directives lower onto `healthEntry` and need the grant check at the language boundary;
  until then P19 8.2's unauthorized-reset gate is met at the host API only (stated).
- If `+pred`/`-pred` should fire on changes of the arbitrated answer rather than of supports, the
  isolate drop's events change meaning in I4; record that as an open point.

**Live-upgrade translator.**

- The profile gains `health_refused`, `health_events`, `fault`, `reasons`, `actions` (and, with M1,
  `reset_epoch`). The translator must copy `fault`, `reasons` and `actions` with their provenance
  and never clear a latch (P19 2.1, "Model reset or program upgrade never clears a hardware fault").
  Absent fields read as initial values (section 4), which makes translation of older profiles safe.
- If a settings bound (`epistemic_health_actions`, `epistemic_health_reasons`) shrinks across an
  upgrade, truncation must follow the same priority as eviction (M2) and must increment
  `actions_evicted`.
- `healthEntry`, `settleEntry` and `discardPendingEntry` must be remapped in `program.cpp` (stated),
  and grants, the latch mirror and the reset epoch are host state outside the arena; on rollback the
  old state must be kept as a unit.

Tally: 0 BLOCKING, 5 MAJOR, 12 MINOR.

Verdict: FAIL
