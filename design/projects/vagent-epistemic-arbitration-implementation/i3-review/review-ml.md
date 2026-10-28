# ML gate review: I3-1 design (checked ingress, per-source observation history)

Read: i3-design.md, ml-gate-plan.md (I3-I6), spec 2.1/2.2/5.1/8.1, I2B carried items, compiler.cpp
emitRtBSet (4120-4210), restoreState (agent_executor.cpp 3385+). No files edited.

Verified against code: rtBSet with `repl` first calls rtBDrop for every OTHER-term clause with the
same key that this source asserts (Unify "same" skips only the identical term); rtBDrop unlinks the
source's `d.source` cell s (and the clause if no source is left). So the support cell that would own
the history is destroyed. restoreState matches predicates by name and copies clauses/terms/sources
as text; it has no epistemic copying yet (design correctly lists it as new work).

## Findings

1. BLOCKING - The dominant ingest case (same source, same key, CHANGED value) is either refused or
   loses the "current" record. Section 4 step 4 reuses rtBSet; for a keyed arbitrated predicate a
   changed value goes through the `repl` path and rtBDrop destroys that source's old support, with
   its `d.epistemic` history. The design refuses "keyed replacement ... that would drop another
   key's support" (the wording is wrong: rtBDrop drops same-key, different-VALUE supports of the
   same source), so continuous readings (position changes every sample) cannot be ingested at all,
   or the record is silently lost. Step 5's "previous record becomes retired" only covers an
   identical tuple, which is the rare case, and the test list has no changed-value case. This
   conflicts with 2.1 "exactly one current observation per source/incarnation and key" and the gate
   "retired observations remain non-voting". Fix: either (a) pull retained-observation transfer for
   the same-source replace path into I3-1 (splice the retired-list under a per-key owner, not the
   support), or (b) state explicitly that I3-1 ingests arbitrated predicates in non-replacing mode
   with a cross-support scan that retires the source's prior current record for the key and add a
   test; (b) must also say what plain queries then see for the old value's clause. Do not leave it
   open. This is also a I4 prerequisite: "current" must be locatable in O(source supports for key).

1. MAJOR - "Current" is per support, not per (principal, incarnation, key). With history hosted on
   the support cell, uniqueness of current across supports for different values of one key is not
   enforced by any step. Fix: define the invariant and the lookup (keyed list or profile-side
   current pointer `d.epistemic` field -> record), test: at most one `current` per (principal,
   incarnation, predicate, key) after every turn.

1. MAJOR - Record lacks the immutable calibration/learning key that 2.1 and I5/I6 require. Only
   `model` and `calibration epoch 0` are stored. Missing: domain/schema ID, model epoch,
   feature-schema epoch/ID, predicate signature (full-key derivation "from its immutable target
   observation" fails), observation version, health state at ingest. Adding fields later is
   migration of semantics, and old records could not be matched to any epoch. Fix: store them now,
   with values for the I3-1 epoch (domain=predicate signature, model epoch 0, feature schema 0),
   plus an explicit observation identity = (principal, incarnation, event ID) and a version integer
   (0).

1. MAJOR - No place for the observation-time feature/context snapshot (I5 gate: "observation-time
   feature snapshots and fixed feature schema/scaler epoch"). Features depend on state at ingest
   (age, innovation, health, variance); they cannot be recomputed later. Fix: reserve an empty
   `features` vector (owned `d.grab`/`d.step` head) with feature schema 0 meaning "no features", so
   I5 can distinguish "ingested before features" (never trainable) from a real snapshot, rather than
   inventing zeros.

1. MAJOR - Verdict `UNTESTED` and re-verdict are unspecified. 5.1 says the verdict is computed at
   ingestion and queries read it; the design does not say whether a later gate may overwrite it.
   Overwriting a record in place corrupts history and breaks replay; leaving I3-1 records `UNTESTED`
   forever is fine only if I4 defines eligibility. Fix: (i) make verdict immutable at ingest; a gate
   version/epoch field (`verdict_epoch`, 0 = no gate) records which gate produced it; (ii) state I4
   rule: UNTESTED is eligible only where the policy has no gates and is never counted as "accepted
   by a gate"; (iii) any re-verdict is an appended verdict revision, not a mutation. Also state that
   I3-1 records must not feed Beta counts (UNTESTED is not a label).

1. MAJOR - Idempotency is weaker than 2.1 / I5 ("changed payload under same ID refuses", "identical
   retry learns once"). Step 1 looks only in the same key's support: same (principal, incarnation,
   eventId) with a different tuple or timestamps is not detected, and an exact retry after the
   record is excised or after a later sequence arrives returns `StaleSequence` instead of an
   idempotent no-op / cached outcome. Fix: store a canonical payload digest on the record (as a
   text/bytes cell, not JSON), keep a per-profile bounded event-ID window (replay horizon), and
   define: same ID+digest => no-op receipt "duplicate"; same ID, different digest =>
   `EventIdCollision`; beyond horizon => StaleSequence by sequence rule. Note that "outcome
   redelivery" (I3 gate) is deferred to I3-4; record that as a gate debt.

1. MAJOR - "No implicit priors" is respected only vacuously. The policy table has no prior
   alpha/beta/decay keys, and records say "calibration epoch 0" while no calibration record exists.
   Spec 2.1 says a new record begins with *declared* priors. Fix: either add required `prior_alpha`,
   `prior_beta` (finite >0) now to the closed key table (policy is recompiled per program, so no
   migration cost), or state that I4 must refuse (`MissingCalibration`) rather than default when no
   record for the full key exists. Say it in the design so I4 cannot adopt Beta(1,1) quietly.

1. MINOR - Numeric types: Double for times/variance and Int64 for sequence/epochs match the carried
   I2B item, but `uint64` host fields must be range-checked on conversion to Int64 (>2^63-1 =>
   InvalidObservation). `receivedAt` is in `CheckedObservation`; it must be stamped by the host from
   its own clock, not source-provided (heartbeat/health later depend on it). `variance` supplied on
   a discrete predicate is unspecified: refuse it. State that variance is squared-units and the
   policy `units` is compared as text exactly (no case folding).

1. MINOR - Bound/history rule. Excising the oldest retired record is consistent with the replay
   horizon only if the per-profile last-sequence guard survives excision (it is on the profile, so
   it does). "Pinned verification targets do not yet exist" is acceptable for I3-1, but I3-4 will
   need a "pinned" flag on the record; reserve it now (Int64 0) or accept a layout bump. 16 retained
   per support also interacts with finding 1 (retired records would need a per-key owner).

1. MINOR - Refusing `retract`/`revise` on arbitrated predicates is acceptable for I3-1 provided the
   refusal is atomic and documented as temporary; but it makes a faulty source's data impossible to
   withdraw until I3-2, and ties I3-1 deployability to I3-2. State that no arbitrated predicate may
   be shipped in a user program before I3-2 (check-runtime feature flag).

1. MINOR - Source profile fields: health flags are stored as NOMINAL/reasons/streak but
   reset/incarnation "invalidates old observations" needs an `incarnation` on each record (present)
   and the profile registry keyed by (principal, incarnation); confirm that a new incarnation does
   not reuse the old profile's last-sequence (it is per profile - good). Add a test: a new
   incarnation with sequence 1 is accepted.

1. MINOR - Bindings are not checkpointed. After resume `observe` refuses `UnknownSource` until the
   host rebinds; an acked-but-uncommitted queued ingestion event at checkpoint time is lost or
   replayed? Specify that queued ingestion events are either drained before checkpoint or the
   receipt is documented as not durable (consistent with P19.5), and test it.

## Sound as designed

- Authorization state outside the arena, receipt not equal commit, all-or-nothing turn with undo,
  fuel failure leaving nothing, no ML state in C++, budget refusal without reclamation, 128th/129th
  source test.
- Per-source supports with independent records; reciprocal edges and fresh clone occurrences per
  record.
- Deferral of gates/priors/learning is reasonable as long as items 3-7 reserve the fields.

## Tally

BLOCKING 1, MAJOR 6, MINOR 5.

VERDICT: FAIL
