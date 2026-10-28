# I3-1 review: hardware, sensors, fault tolerance

Sources: i3-design.md (sections 2-8), spec 2.1, 3.2, 5.1, 5.2, 7, 8.1.

## BLOCKING

1. **Reboot lock-out; nobody owns the incarnation bump.** Section 4 step 2 refuses any
   `sequence <= last` per (principal, incarnation) profile. A rebooted device restarts its counter,
   so every reading is `StaleSequence` until the host rebinds with a higher incarnation. The grant
   says the incarnation "bumps on device replacement or reset", but section 3 names no detector, no
   rule, and no handling for a reboot with no replacement. Refused turns undo (step 2), so they
   leave no reject count and no last-receive update (see 7). The failure is silent and permanent.
   Fix: define the incarnation as a host-owned boot or session counter that the sensor adapter
   authenticates. Specify that a bind with a higher incarnation supersedes and revokes the lower
   one. Return a distinct `SequenceRegression` refusal from `observe()` before any turn, and have
   the host act on it.

1. **Continuous predicates collide with keyed replacement.** Step 4 reuses `rtBSet` keyed coalescing
   and keys history by "the same tuple". A continuous reading carries a Float value, so two
   successive readings never share a tuple. Either the new value replaces the old clause, which
   drops the support cell that holds the history (so the 16-record bound never matters), or it is a
   different clause. Under the section 4 closing paragraph, a second source's differing value is a
   "keyed replacement that would drop another support" and refuses `UnsupportedEpistemicChange`.
   That makes GPS plus odometry on one key, the motivating case, unusable in I3-1. The test in
   section 8 ("same tuple retires its previous record") only covers discrete predicates. Fix: state
   the semantics for continuous predicates. Key the support by (key args, source), with the value
   living in the record rather than in the clause identity. Add a two-source, differing-value test.

## MAJOR

3. **Sequence scope is undefined.** A profile holds one `last sequence`, but a grant covers one
   predicate and one principal can hold several grants (a GNSS receiver reporting position and
   velocity). Two interleaved streams sharing one strict `>` check refuse each other. Fix: scope
   sequence to (principal, incarnation, predicate), or require one counter per principal and say so.

1. **Event ID reuse across incarnations.** Idempotency (step 1) looks at the support's history. The
   support is named by the alias, and the profile is named by incarnation. If a rebooted or replaced
   device reuses event IDs (counter-style IDs "1", "2"), the new reading matches a record from the
   old incarnation. It is silently dropped as a retry while the receipt reports success. Fix: scope
   the dedupe key to (incarnation, eventId), or make IDs globally unique and checked at `observe()`.
   Also say what a retry of an event excised from the 16-record window does. As written it falls
   through to `StaleSequence`, which is acceptable but undocumented.

1. **Profile churn and latch laundering.** Each incarnation creates a new profile with NOMINAL
   health. `max_sources` (128) is consumed per reboot and reclaims nothing in I3-1, so a flapping
   device exhausts the budget and starves real sources (`EpistemicBudget`). A fresh NOMINAL profile
   also lets a FAULTED, isolated device shed its latch by restarting, contradicting 5.1 ("only
   authorized `reset` unlatches"). Fix: link the new profile to its predecessor, inherit the latch,
   and count (principal) rather than (principal, incarnation) against the budget. Or reclaim
   superseded incarnation profiles in I3-1.

1. **Time checks are missing; clock epoch is only stored.** The design validates "finite and
   uncertainty >= 0" only. Spec 3.2 requires rejecting negative ages, future timestamps beyond
   tolerance, and unknown or changed clock mappings as `ObservationTimeMismatch`. That code does not
   appear in section 3. Nothing compares `clockEpoch` or `clockMappingVersion` with the previous
   value, because the profile stores neither. Nothing bounds `acquiredAt` against `receivedAt`. The
   record holds only a mapping version number, with no addressable snapshot until I3-3. That breaks
   the "preserve for the full retained lifetime" rule, so records written now will be unresolvable.
   Fix: add `ObservationTimeMismatch`; keep the last epoch, mapping version and acquisition time on
   the profile; refuse an unknown mapping version at ingress; add a policy `future_tolerance`; give
   I3-1 a minimal mapping table cell, or state that I3-1 records are tagged "mapping unavailable".

1. **Rejections are invisible to health and liveness.** The profile's reject count and last receive
   are only updated in committed turns, but `StaleSequence`, budget and fuel failures undo the turn.
   Refusals at `observe()` never reach a profile. A live device being rejected looks silent to the
   I3-3 heartbeat and will be marked timed out. The cause is a protocol or config fault, not silence
   (5.1: "silence establishes missing data, not its cause"). Fix: maintain host-side counters
   (reject count, last-receive by cause) outside the undone turn, folded into the profile by a
   separate small health turn. Or commit a bounded rejection record.

1. **Ingestion queue: no bound, no backpressure, no failure channel.** One wake turn per
   observation, unbounded queue, "accepts on receipt". A host flood grows memory without limit and
   starves other deliveries. A queued turn that later fails (`EpistemicBudget`, fuel, grant expiry)
   after a success receipt gives the host no event to retry on. Deterministic fuel exhaustion will
   poison-retry. Retries reordered behind later sequences become `StaleSequence` and are lost. Fix:
   add a per-source queue cap with `Backpressure` refusal at `observe()`. Add an explicit completion
   or failure callback or percept keyed by eventId. Re-check the grant expiry at turn time. Specify
   a per-observation fuel bound. Measure turn cost, including the O(L) `cloneMaster` walk over all
   of a profile's clone occurrences.

1. **History bound versus high-rate sensors.** 16 records per support at 100-200 Hz IMU is under 0.2
   s. `alignment`, `max_age`, and verification delay all exceed that, and retired records are
   excised eagerly with no pin until I3-4. So labels and replay cannot reference a past observation,
   and the alignment target `t_star` may precede the oldest retained record. Fix: bound by time
   horizon (`max(max_age, alignment, verification_delay)`) as well as count, or state that the
   history is not a replay store. Report the cell cost per record (16 x keys x 128 sources) against
   arena limits.

1. **Variance validation is too weak.** `InvalidVariance` is "non-finite or nonpositive". Subnormal
   values (1e-320) and huge values (1e300) pass. They overflow `r/max(w,eps)` and `d^2`, and a
   precision sum goes to infinity (8.1: "refuses rather than invents confidence"). Fix: require
   `min_variance <= v <= max_variance` as policy fields, with normal-range doubles only. Refuse a
   variance supplied for a non-continuous predicate. The units check is string equality only, so "m"
   and "metre" mismatch. Say that the policy units string is canonical and that no conversion is
   attempted.

## MINOR

11. **Frame and unit refusals** (`ObservationFrameMismatch`) before any turn leave no trace. After a
    device swap with a different frame, this misconfiguration is invisible. Count them per principal
    (see 7).

01. **Profile is too thin for I3-2 and I3-3.** Needed beyond the initial fields:

    - a FAULTED latch distinct from the displayed state;
    - separate timeout and freeze flags;
    - heartbeat interval and deadline;
    - last acquisition time, epoch and mapping version;
    - degraded and suspect reasons list;
    - correction epoch;
    - calibration-record head (required by 2.1).

    I2U refuses populated roots and there is no translator, so adding these later forces a
    migration. Create the full set at profile creation.

01. **Grant `expiresAt` uses `system_clock`**, which steps under NTP. Use a monotonic or actor
    basis. The grant has no revoke call, so `unbind` should exist.

01. **Section 4 order.** Idempotency then sequence then profile creation: a retry of an old event
    whose profile is already at the `max_sources` budget still evaluates correctly, but state this
    explicitly.

01. **`receivedAt` is host-supplied and trusted for liveness.** Stamp it on the actor side at
    `observe()`, or document the host as trusted.

## Tally

BLOCKING 2, MAJOR 8, MINOR 5.

VERDICT: FAIL
