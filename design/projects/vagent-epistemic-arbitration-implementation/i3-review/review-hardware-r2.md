# I3-1 revision 2 review: hardware, sensors, fault tolerance

Read: whole of i3-design.md (revision 2) against spec 2.1, 3.2, 5.1, 5.2, 7, 8.1.

## Round-1 resolution check

- **Resolved:**
  - Own-source transfer for continuous readings: one support per (identity, key), history spliced to
    the new support (5.4).
  - Per-stream sequences (5.2).
  - Dedup with digest, window and `ConflictingEvent`. It is held per profile, so event IDs no longer
    collide across incarnations.
  - Executor-stamped `receivedAt`, and grant expiry on the actor clock.
  - `ObservationTimeMismatch`, the outcome log, and the `IngressBackpressure` refusal.
  - Latch and reason inheritance on a new incarnation.
  - Principals, not incarnations, counted against `max_sources`.
  - Profile fields in section 7.
- **Partly resolved:** reject bookkeeping, variance domain, history/checkpoint budget, incarnation
  ownership. See below.

## BLOCKING

None.

## MAJOR

1. **Incarnation lifecycle is incomplete.**

   - Section 3 says a new incarnation creates a profile linked to its predecessor. It never says
     what `observe`/`bindSource` do with a *lower* incarnation after a higher one has been seen. A
     replayed or zombie old device is bound or unbound at the host's whim.
   - `bindSource` refuses "a second alias for the same (principal, predicate)", but no `unbind` or
     supersede call exists. Moving to incarnation N+1 needs the old binding gone first, and nothing
     says who removes it, so reboot recovery is not specified end to end.
   - Fix:
     - Rule that a bind with a higher incarnation atomically supersedes the lower one.
     - Refuse any lower incarnation (`StaleIncarnation`).
     - Add `unbindSource`.
     - Test the zombie-after-replacement case.

1. **The predecessor's records and supports survive.**

   - Supports are keyed `<principal>#<incarnation>`, so the new incarnation's support for a key is a
     different cell from the predecessor's.
   - "The predecessor's current records are retired by that ingestion" only covers the key being
     reported. Records for keys the new incarnation never reports stay `current`. Even for the
     reported key, retiring a record does not say the old support is dropped, so the old value's
     clause may stay supported by a dead incarnation.
   - Until I3-3 nothing expires them, so a replaced IMU or GNSS keeps contributing stale readings
     for the other keys, and the same principal counts twice.
   - Fix: on first ingestion of a new incarnation, retire all predecessor current records. Specify
     whether the old supports are dropped (the clause then follows the normal `-pred` events) or
     transferred. Add a two-incarnation, two-key test.

1. **Ingress queue is per actor, so a babbling sensor starves the rest.**

   - The cap of 64 is shared by all sources. A stuck or looping device sending at kHz fills it and
     every healthy source gets `IngressBackpressure`.
   - Rejected-observation bookkeeping adds a second write per failure, doubling cost under exactly
     that flood.
   - Related: the epistemic cell budget (point 5) is first-come, so a noisy source can exhaust it
     and starve later principals, with no reclamation.
   - Fix: per-source (principal, predicate) queue quotas in addition to the global cap, and coalesce
     the bookkeeping write per source per drain. Define a per-principal share of the cell budget.

1. **Variance has a floor but no ceiling.** "Normal positive finite" still admits 1e300 or DBL_MAX.

   - Then `r/max(w, eps_floor)` and precision sums overflow to infinity in I4. Under 8.1 that is a
     refusal, so one compromised or buggy bound source with a huge variance makes the query for that
     key fail for every source.
   - Fix: policy `min_variance` and `max_variance`, refused at ingress (`InvalidVariance`), with the
     I4 arithmetic proven safe inside that range.

1. **The cell budget is computed per principal, but the cost is per support (identity x key).**

   - Section 8's 8 x 60 x 128 = 61k assumes one key per principal. A source reporting ten keys costs
     ten times as much.
   - Dedup windows (64 pairs per profile), streams, tuple views and profiles are not counted. With
     the defaults, 128 principals cannot be reached near the 65,536 cell checkpoint budget, and the
     test "129th principal refuses `EpistemicBudget`" would be refused earlier by the cell budget
     instead of by principal count.
   - Fix: give a cost model per (principal, key) plus fixed per-profile cost. Set defaults so
     max_sources x keys fits, or lower `max_sources`. Make the test reflect which limit trips.

## MINOR

6. **Ingress-level refusals for bound sources** (frame/units, time, variance, backpressure) happen
   before any turn, and the bookkeeping write is described only for turn failures. A bound device
   failing validation at `observe()` therefore does not update reject count or last contact, and
   I3-3 will see it as silent. Also, a first-ever refused observation has no profile to update. Fix:
   route all bound-source refusals through the same bookkeeping, or keep host-side counters.

1. **Duplicate commits nothing**, including last contact. A retrying device is alive, so a
   `Duplicate` should refresh last contact.

1. **Time checks are still thin.**

   - "Acquisition later than receipt by more than its uncertainty" has no policy skew tolerance, so
     a device reporting uncertainty 0 with a few ms of skew is refused (spec 3.2 says "policy
     tolerance"). Add `future_tolerance`.
   - No check that the policy clock basis is comparable with the executor clock used for the
     comparison.
   - Clock epoch and mapping version are stored but never compared with the stream's previous
     values. Section 7 lacks last acquisition time, epoch and mapping version, so the I3-3 slew gate
     and epoch-change handling will have to add stream fields.
   - Fix: add these fields to the stream record now, and state that I4 treats a record whose mapping
     version cannot be resolved as ineligible.

1. **Checkpoint drain** ("ingestion events drain before a checkpoint is accepted") can starve a
   checkpoint under flood. Bound the drain or checkpoint the queue-empty condition with a retry.

1. **History 8 is documented as not a replay store.** Fine for I3-1, but state that pinning in I3-4
   must precede any use of labels, since retired unpinned records are excised eagerly now.

## Tally

BLOCKING 0, MAJOR 5, MINOR 5.

VERDICT: FAIL
