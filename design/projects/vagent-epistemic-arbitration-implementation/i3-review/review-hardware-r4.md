# I3-1 revision 4 review: hardware, sensors, fault tolerance

Read the whole of i3-design.md (revision 4) against spec 2.1, 2.2, 3.2, 5.1, 5.2, 7, 8.1.

## Round-3 findings: verification

- **M1, dedup versus incarnation: resolved.** The incarnation step precedes dedup, and the window is
  keyed by (incarnation, ID) and cleared on advance.
- **M2, budget leak: logical budget resolved.** `retireSource` returns the logical cell count. See
  major 2 for the physical side.
- **M3, wall clock: partly resolved.**
  - Grants and liveness now use the steady clock.
  - `ClockUnsynchronised` is retryable and not charged.
  - The wall clock is still the reference for the acquisition check (major 1).
- **M4, per-key ordering: resolved.** Order is compared per (principal, key), with epoch and mapping
  regressions refused.
- **Other round-3 items:** round-robin drain by principal, `max_variance` ceiling (but see minor 5),
  and 1b shipping with 1c are all resolved.

## BLOCKING

None.

## MAJOR

1. **The acquisition check is tied to the wall clock regardless of the policy clock basis.**

   - `ObservationTimeMismatch` compares `acquiredAt`, which is "mapped seconds, policy clock basis",
     with `receivedWall`.
   - `ClockUnsynchronised` then refuses every observation, of every predicate and basis, while the
     host reports the wall clock unsynchronised.
   - A vehicle or field node with no network time, a sensor mapped to a steady or GNSS-disciplined
     basis, or an RTC-less board that never synchronises, either cannot ingest at all or must lie to
     the executor via `setWallClockSynchronised(true)`. The refusal is Retryable, so the host
     retries a permanent condition forever.
   - It also freezes unrelated sensors.
   - Fix:
     - The policy `clock` key names a basis (`wall` or `steady`, with a mapping contract).
     - Compare acquisition with the receipt stamp of that basis (`receivedSteady` for steady).
     - Apply `ClockUnsynchronised` only to wall-basis predicates, and only per predicate.
     - Add a test with a steady-basis predicate and an unsynchronised wall clock.

1. **Physical arena exhaustion is not priced; the budget returned is only logical.**

   - Spec 2.2 says unlinking "does not reclaim arena slots", that tombstones keep counting against
     the physical cell budget, and that churn "may exhaust it even with few live sources".
     Reclamation needs a quiescent reachable-graph rebuild that is explicitly not built.
   - Section 6 step 9 and `retireSource` treat excision as returning budget, and the design never
     states the physical rate.
   - At about 100 cells per record and a modest 10 Hz source, steady state mints about 1,000
     physical cells per second. Any finite arena is exhausted in minutes to hours, whatever
     `vagent.epistemic_cells` says.
   - The resulting refusal is classed Retryable and would never clear.
   - Fix:
     - State the arena capacity and a computed time-to-exhaustion at the stated ingest rates, per
       slice.
     - Add a physical-slot counter distinct from `$epistemic_cells`.
     - Classify a physical-budget refusal separately and Terminal for the actor, with a defined host
       action (rebuild at checkpoint, or restart from checkpoint).
     - Either bound the sustained ingest rate per source (upstream estimator rate limit, enforced)
       or record that 1b is a demonstration-rate slice until the rebuild mechanism exists.

## MINOR

3. **Duplicate contact is inconsistent.** Section 6 step 3 says `Duplicate` "refreshes and commits
   nothing", and step 7 refreshes contact only for committed events. The 1b test row still says
   "retry `Duplicate` refreshes only contact". A device whose receipts were lost and which
   retransmits only duplicates will look silent to I3-3. Decide one rule and fix the test row.

1. **A reboot without an incarnation bump is indistinguishable from silence.** Direct
   `StaleSequence` refusals charge `refused_count`, which never refreshes contact, so I3-3 will time
   out a live device that is simply regressing. This is deliberate for replay safety, but there is
   no health-visible signal. Add a `sequence_regression_streak` (or reason hint) so the operator
   sees "alive, sequence regressed, bump incarnation".

1. **Variance bound claim is false for sums.**

   - `DBL_MIN <= min` guarantees a single reciprocal is finite, but about 4.5e307 times a handful of
     sources overflows a precision sum. `r/max(w, eps)` is worse.
   - A square of `DBL_MIN` underflows to zero.
   - Also, a variance range from 2e-308 to 1e150 is not a physical sensor domain.
   - Fix: choose a conservative engineering range such as [1e-100, 1e100], or state the bound I4
     proves.

1. **Retiring a principal erases its incarnation floor.** After `retireSource` and a rebind, a
   zombie device with an older incarnation can be bound. Keep a tombstone incarnation floor in
   executor state (the grant table), not the arena.

1. **Per-key acquisition tolerance** (two uncertainties plus skew) means an older reading inside the
   tolerance still replaces the newer one as the current record. Acceptable, but document that
   `current` means "latest received", not "latest acquired".

1. `setWallClockSynchronised` is host-asserted, and "after a detected step" has no detector owner.
   Name it.

## Tally

BLOCKING 0, MAJOR 2, MINOR 6.

VERDICT: FAIL
