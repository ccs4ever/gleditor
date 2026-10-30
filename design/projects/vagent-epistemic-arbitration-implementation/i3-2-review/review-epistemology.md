# I3-2 design review: epistemology

Scope: `i3-2-design.md` revision 1 at commit e226c507, read against P19 sections 2.1, 4 and 5.1, the
owner's Q2 ruling (taken as given), I3-1 section 14, and `compiler.cpp` (`emitProfileFor` 4432,
`emitObserveRefusal` 4497, `emitSourceChange`), `agent_executor.cpp` (`pushControl` 592, `charge`
825, `isContentRefusal` 845, `restoreState` 4504-4625). Persona: what the system claims to know, and
on whose authority.

## BLOCKING

None.

## MAJOR

**M1. A reset can be undone by a reading received before it (section 6.2, 6.3; `agent_executor.cpp:592`).**
`pushControl` inserts the health turn ahead of every non-sourceChange post, so a `Reset` overtakes
observations that were received and queued earlier. Those run after the reset, pass the dedup floor
(they are not replays; they were never ingested), and become the "new eligible observation" of a
source whose old observations were just invalidated. Scenario: an isolated, drifting sensor has
readings r1..r3 queued; the operator resets; r1..r3 are ingested after the reset and the source is
eligible again on pre-reset evidence. P19 5.1 requires a *new* observation after reset. The same
ordering is right for an advance (the observation carries an incarnation, so a stale one refuses) but
there is no such tag for a reset, which does not change the incarnation.
Remedy: either queue `Reset` in FIFO order with observations (use `push`, keep `pushControl` for
`Isolate`/`Degrade` only, where overtaking is fail-safe), or give the profile a reset floor (the
steady time of the reset turn) and refuse ingestion of readings with `received_steady` below it,
rebased on resume like the other steady fields. Add the test: queue r1, reset, assert r1 is not
current afterwards.

**M2. SUSPECT is derived from refusals that are not evidence against the source (section 3.1, 4;
`agent_executor.cpp:845`).** `isContentRefusal` includes `StaleObservation` and
`ObservationTimeMismatch`. A congested link, a clock-mapping fault in the host, or a late-delivered
batch makes every affected source read SUSPECT, with nothing in the state naming the cause. P19 5.1
says silence "establishes missing data, not its cause", that timeout flags "affect eligibility
separately from gate rejection counts", and `chargeable()` itself already excludes the host's own
conditions (`ClockUnsynchronised`). SUSPECT carries inflation 4 in the spec, so this is a claim about
the data, made on latency. Common-cause: one host fault moves all sources at once, which looks like
a fleet failure.
Remedy: feed health only from codes that show the device sent unusable content (frame, missing or
invalid variance, uncertain acquisition); count staleness and time mismatch into `refused_count`
(or a third diagnostic counter) until I3-3 can tell device lateness from path lateness. If the owner
wants them in, say so in section 3.1 and add a common-cause guard test (all sources suspect at once).

**M3. Reset and isolate leave no durable account of who did what (sections 5, 6.3).** Reset "clears
the latch and its provenance, empties `reasons`". After it, the profile cannot say that a fault
existed, who latched it, who cleared it, or when. P19 2.1 requires preserving "warning/suspicion
reasons and their provenance"; a reset is the event an audit most needs. `at_wall` is also the only
time on a reason, and the reason has no incarnation, so a degrade placed on incarnation 3 silently
applies to a replacement device at incarnation 4 (and the spec reserves carrying a physical fault
into a replacement for the host's checked policy). A second isolate "adds nothing", so a different
operator's second isolate is also lost.
Remedy: add a small bounded owned `actions` list (kind, code, grant, issuer, incarnation, steady and
wall time, and for a reset the prior latch provenance), appended by every accepted action and never
cleared by reset; budget it with `epistemic_health_reasons`. Record `incarnation` on reasons and
fault. State what an advance does to reasons (keep, matching the latch) and test it.

**M4. FAULTED changes no answer in I3-2 (section 6.3, "Supports stay").** Isolate sets a flag and a
stamp; plain queries still read the isolated source's supports, and ingestion keeps accepting and
supporting new readings from it. P19 5.1: "Active fault status excludes all source readings." The
design defers the exclusion to I4, but ships an operator action named isolate and a state named
FAULTED. Between I3-2 and I4 the system displays a claim ("this source is excluded") that is false of
its own beliefs. The deferral costs nothing recoverable: the only exit from FAULTED is a reset, and
reset invalidates every old reading anyway, so withdrawing supports at isolate loses nothing.
Remedy: either (a) at isolate, drop the source's supports through `rtBDrop` (records go to the
retired chain as a reset does) and have ingestion record but not support while latched; or (b)
declare in section 6.3 and the acceptance text that I3-2's FAULTED affects only state and stamp, name
the hole, and test that the belief still stands. (a) is preferred. If (a), the section 10 wedge
applies to isolate: set the latch before the drop is attempted, in the same turn but with the drop
failure leaving the latch set, or the safety action inherits the liveness defect of reset.

**M5. Isolate must not be refusable by resource pressure (sections 6.3 end, 5).** Isolate goes
through `emitProfileFor` (`compiler.cpp:4432`), which ends `EpistemicBudget` at `max_sources`, and
writes about 12 cells of `fault` provenance, so it is gated by `ArenaExhausted` and the cell ceiling.
The action that matters most under fault is the one that fails when the arena is full. Remedy: set
the existing `faulted` bool first and make provenance best-effort or preallocated at profile
creation; for an unbound principal with a tombstone, allow isolate on the tombstone; for one with no
profile, say what happens (see m3).

**M6. Stale decay evidence across a checkpoint (section 3.5).** `c` is stored as of `t_c`, not as of
the checkpoint. Rebasing `t_c` to the restore time throws away the decay between the last charge and
the checkpoint, so a source quiet for ten half-lives before checkpointing resumes at its full old
count (SUSPECT at c=3 stays SUSPECT). Repeated restarts every few seconds then pin it SUSPECT
indefinitely. The justification (no laundering by downtime) covers the downtime, not the pre-save
quiet. Remedy: at checkpoint, materialise each counter to the checkpoint's steady time
(`c * 2^(-(now - t_c)/h)`), then at restore set `t_c` to the restore time. Add a test with a long
quiet before save. If a trusted clock mapping exists later (I3-3), credit downtime from it as P19
2.1 allows for heartbeats, and keep "unknown mapping never clears a latch".

## MINOR

- **m1. Self-issued health actions (section 6.2).** `issuer` is a claim compared with a string the
  host also supplied; the check is only as strong as the adapter. Add the explicit rule that
  `issuer` must not equal the `principal` acted on or any principal bound to the actor, so a sensor
  identity cannot hold or exercise a grant over itself (P19 4: a sensor "cannot clear its own
  latched fault"). Test it for Reset in 2b even though sensor reports arrive in I3-3.
- **m2. Grant checked once, at staging (6.1, 6.2).** `revokeHealthGrant` does not cancel queued
  health turns, and expiry is read at call time, not turn time. Acceptable (P19 4 says "before
  staging") but write it down, and have the outcome carry the grant ID so a revoked-then-run action
  is visible.
- **m3. Unbound principal with no profile refuses `NoProfile` (6.3).** An operator cannot isolate a
  known-bad device before it binds, and the first bind starts clean. That conflicts with the spirit
  of "latch for life". Either allow an isolate to mint a profile (bounded by `max_sources`, M5) or
  record the limit.
- **m4. Reasons have no identity (section 5).** A repeat `degrade` with the same code appends again
  and exhausts the bound of 8, after which a legitimate new reason refuses `EpistemicBudget` and
  only a full reset clears the list. Make (kind, code) unique per profile (a repeat keeps the first
  provenance, as isolate does) so a later recovery percept has one "corresponding reason" to clear.
- **m5. Derived SUSPECT has no reason record (section 4).** P19 keeps "suspicion reasons and their
  provenance". A rate-derived SUSPECT shows no cause. Have `rtHealth` return the winning reason code
  (latch provenance, a stored reason's code, or `content_refusal_rate` with `last_refusal`) so
  displays and audit can say why.
- **m6. Stamp versus live state (section 4).** The stamp is the health at receipt; I4's eligibility
  must read live state, and inflation must be applied once. Say which consumer reads which, or I4
  will read `record.health` for eligibility and keep an isolated source's earlier NOMINAL records
  eligible.
- **m7. `rejection_streak` is not read by `rtHealth`.** P19 5.1 makes streak 3 SUSPECT and 5 FAULT.
  Deferral is fine (no producer; I4 clears it), but record it in section 3.3 so the state rule is
  not believed complete.
- **m8. Settings are outside provenance (3.3).** A change to half-life or threshold changes derived
  state for stored counters without a record. State that the stamp is the state under the settings
  at ingest, and that the settings are owner-written system xanadoc values.
- **m9. Reboot test is ambiguous (section 8).** "A reboot keeps the latch" needs three cases: source
  reboot (advance to a higher incarnation), retire then revive, and executor checkpoint/resume.
  The first is the P19 2.1 rule ("a source reboot alone is not reset authority") and is the one at
  risk if the shared advance body is parameterised for reset.
- **m10. Refusal-only profile (section 4).** The statement is sound (no record, nothing eligible,
  absent `last_receive` is not 0, counters and latch apply) and honours the sentinel rule. Add that
  a reset of it is valid and retires nothing, and that its state can be SUSPECT with no reading
  ever accepted; test both.

## Section 9 deferrals

- Cross-key retained transfer: nothing lost. Reset moves records to the existing retired chain, and
  I4 re-homes them. Record as an I4 obligation that retained IDs, outcomes and floors survive the
  move (P19 2.1).
- User-level revision: unrelated to health; nothing lost.
- Sensor health reports: acceptable only because the list shape and provenance fit them. Their
  rule ("a sensor cannot clear its own fault") must hold from 2b (m1), and a recovery percept should
  clear only reasons that source added, never an operator's (M3 provenance enables this).
- Language directives: nothing lost, but the actor-level gate "unauthorized reset rejected" (P19
  section 8.2) is then met only at the C++ API; say so in the acceptance text.
- Refusal-only profile state is described correctly. The latch/reasons derivation matches 5.1 order
  (FAULTED, SUSPECT, DEGRADED, NOMINAL); accepted measurements clearing only the streak is untouched
  because I3-2 clears nothing; reset keeps the dedup floor and streams, so replay still refuses
  (M1 is the exception: a not-yet-ingested reading is not a replay).

## Section 10 answers

1. Half-life and hysteresis: ML's gate. From my seat, no hysteresis; derived state with no stored
   flag has no event to miss, which is the right authority property. The threshold question is
   moot until M2 fixes which refusals count.
1. Clearing a degrade without reset: not in I3-2. P19 lists three directives, and a fourth is a
   spec change. A full reset to clear a warning is too blunt (it discards learned state and
   invalidates readings, and P19 says unrelated warnings survive model resets), so the answer is
   the recovery percept of I3-3, scoped to reasons that source added, plus, if the owner wants
   an operator path, an `undegrade` action under the existing `Degrade` grant that removes one
   (kind, code) reason and appends an `actions` entry (M3). Until then, make reasons unique (m4)
   so the bound is not exhausted by repeats, and note the bound is a real ceiling operators hit
   only by repeating themselves.
1. Wedge: reset refusing when a `-pred` handler fails is fail-closed and correct (latch stays,
   nothing half-cleared). It becomes an isolate problem only if M4(a) is adopted; see M4.

Verdict: FAIL
