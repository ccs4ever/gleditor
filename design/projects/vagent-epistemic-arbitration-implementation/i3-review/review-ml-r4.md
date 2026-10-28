# ML gate review, round 4: I3-1 design revision 4

Read in full (288+ lines, sections 1-12) against the round-3 report, ml-gate-plan I3-I6 and spec
2.1/2.2/5.1/8.1. No repository files edited.

## Round-3 findings: status

1. MAJOR dedup before incarnation: RESOLVED. Section 6 step 2 (incarnation, advance first) precedes
   step 3 (dedup keyed by (incarnation, event ID, digest), cleared on advance). A rebooted device
   reusing an event ID meets an empty window. Test row for 1c should name this case explicitly (see
   finding 3).
1. MAJOR per-key acquisition order: RESOLVED. Step 5 compares with this principal's current record
   for the same key, same clock epoch, with both uncertainties plus `clock_skew` tolerance; the
   stream keeps transport order only; a new clock epoch resets the comparison. 3-7 minors: RESOLVED.
   Eligibility is "current and incarnation equals profile's", profile resolved once per support;
   advance turns retire lower-incarnation records eagerly on every key (O(K\*H)); Duplicate
   refreshes nothing and only direct committed events refresh contact; admission is on the net
   increase after excision; calibration expiry for lower incarnations is explicitly handed to I4
   with P19 5.1.

## I4/I5 sufficiency

Sufficient. One current record per (principal, key) across incarnations holds by construction
(principal-keyed support, retire-on-advance). The record carries the full immutable calibration key
(predicate signature, domain/schema ID = signature + policy hash, model and calibration epochs,
feature schema 0, incarnation, observation version), an empty feature list, immutable verdict with
epoch and an empty `verdict_revisions` head (a later gate can re-verdict by appending), pinned flag,
health at ingest, digest, and both receipt times. Declared priors are required; UNTESTED is
"unscored, not endorsed". Numeric types match 8.1 (Double times and variance, Int64
sequences/epochs, uint64 range check). No implicit priors, no sentinels.

## Findings (none blocking)

1. MINOR - Variance lower bound still allows overflow downstream. `DBL_MIN <= min` guarantees a
   reciprocal is finite, but the precision 1/v of four sources at DBL_MIN (about 4.5e307 each)
   overflows when summed, so I4's precision sums would refuse on a policy-legal input. Fix: bound
   `min_variance >= 1e-150` (symmetric with the upper bound), which keeps sums of millions of
   sources finite; keep "normal positive". I4 still tests the non-finite refusal.

1. MINOR - Steady-clock receipt times in checkpointed state. `receivedSteady` (and profile last
   receive/contact, all Double) come from `clock_.steady()`, whose origin is per boot/host. After
   resume on another host they are incomparable, so I3-3 liveness could treat a source as
   permanently fresh or stale. Fix before 1d: record a receipt-clock instance/epoch id with them and
   rebase or invalidate them on restore (treat restored contacts as "unknown, awaiting first
   contact"), with a 1d test.

1. MINOR - Window between binding supersession and the advance turn. Supersession is atomic in the
   executor but the advance turn is queued; a query or ingestion turn scheduled between could see
   the old incarnation's records as eligible. The advance-in-observation fallback covers ingestion
   only. Fix: give the advance turn queue priority ahead of queries/ingestion for that principal, or
   complete it before the binding takes effect; add a 1c test with a query enqueued immediately
   after rebinding.

1. MINOR - Per-key tolerance means a within-tolerance earlier reading can retire a later one for the
   same key. Acceptable by design (clock uncertainty), but state it, and require I4 to age from
   acquisition time rather than from record order.

1. MINOR - `retireSource` excises the profile and its incarnation floor. A decommissioned device
   that later returns with a lower incarnation would be accepted as new (no floor). Keep a bounded
   tombstone floor (principal, highest incarnation) or require re-commissioning to a higher
   incarnation.

1. MINOR - Stale text: the section 11 1b row says "retry `Duplicate` refreshes only contact"
   (contradicts step 3 and step 7: refreshes nothing); section 9 says "dedup pairs" while the window
   holds triples. Align before implementation, since tests are derived from that row.

## Verdict on slices

I3-1a, 1b and 1c may proceed (1b ships with 1c, and starts with the turn-result spike). Findings 1,
3 and 6 should be applied in the 1b/1c implementation; 2 and 5 before 1d. 1d remains subject to the
restore spike review as the design already states.

Tally: BLOCKING 0, MAJOR 0, MINOR 6.

VERDICT: PASS
