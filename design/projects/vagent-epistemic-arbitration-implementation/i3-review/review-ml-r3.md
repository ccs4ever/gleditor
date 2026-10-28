# ML gate review, round 3: I3-1 design revision 3

Read in full: i3-design.md (rev 3) against the round-2 report, ml-gate-plan I3-I6 and spec
2.1/2.2/5.1/8.1. No repository files edited. Round-2 minors 1-6 are folded in correctly
(verdict_revisions list, separate counters, executor-computed canonical digest, domain/schema ID,
lower-incarnation retirement, honest cell count). The changes below were re-judged for regressions
against I4/I5.

## Checked and accepted

- Principal-keyed supports with incarnation on profile and every record: gives at most one support,
  hence one `current` record, per (principal, key) across incarnations, and the calibration key
  still carries incarnation through the record. Sound for I5 exact-match derivation from the target
  observation.
- `min_variance`/`max_variance` refusals are explicit (no clamping); consistent with 8.1.
- history 4 / dedup 16 / cell ceiling with maintained counter: safe by explicit `EpistemicBudget`.
- content_refusals vs refused_count vs gate reject_count keep competence/health separate; relays
  charge only the diagnostic counter.

## Findings

1. MAJOR - Dedup runs before the incarnation check, so a rebooted device is locked out by the very
   case the design allows. Section 3 says a restarting device "might reuse event IDs" and must raise
   its incarnation; section 6 step 2 matches the window on event ID alone (digest differs because it
   includes incarnation), so a reused ID after a reboot returns `ConflictingEvent` (Terminal) before
   step 3 can advance the incarnation. Fix: run the incarnation step first and key window pairs by
   (incarnation, event ID); clear the window when the profile advances. Add a 1c test: new
   incarnation reusing a previous event ID with a different payload commits.

1. MAJOR - The acquisition-order rule is scoped to the stream (profile, predicate), not the key. A
   principal reporting several keys (a gateway, or channels with unequal latency) has a reading for
   key B acquired at t=9 refused `ObservationTimeMismatch` (Terminal) merely because key A was
   acquired at t=10 earlier in the stream. That is silent loss of valid evidence before I4 sees it.
   The guard's stated purpose ("a higher sequence can never retire a newer reading with an older
   one") is per key. Fix: compare against the current record of the same (principal, key) (already
   at hand through the support), keep the clock-epoch/mapping- version exemption, and keep the
   stream for sequence only. Add a test: interleaved keys with older cross-key acquisition commit;
   older same-key acquisition refuses.

1. MINOR - Eligibility rule cost is understated. "One comparison per record" omits resolving the
   profile's incarnation. A record's profile occurrence resolves to its master only by
   `cloneMaster()`, O(L) in occurrences (spec 2.1). Fix: state I4 resolves the profile once per
   support (the support text is the token; profile lookup O(S)) and then compares each record's
   integer incarnation. Specify that status `current` is necessary but not sufficient: invariant
   tests must read "current and incarnation equals the profile's".

1. MINOR - Lazy retirement leaves a leak and a stale vote. A lower-incarnation current record on a
   key the device never reports again is never retired or excised, keeps its cells against
   `epistemic_cells`, and its support still stands under the clause for plain `believes`/`+pred`
   readers. Acceptable for I3-1 if documented; add a note that I3-2/I3-3 provide the reclaiming
   path, and that I4 reads only through the eligibility rule.

1. MINOR - `Duplicate` refreshes last contact even for a relayed copy. A hostile relay replaying a
   signed old event can make a silent sensor appear alive to I3-3 heartbeat. Fix: refresh contact
   only for events delivered directly by the origin (relayedBy absent); a relayed duplicate touches
   nothing but the outcome log.

1. MINOR - Admission arithmetic: at history cap, the append and excision are net zero cells; state
   the `$epistemic_cells` check is on the net increase, so a steady source is not refused because
   others' retired history fills the ceiling (excise own retired record first, then test). Also
   require `min_variance` to be at least the smallest normal double (8.1's "normal positive" rule),
   and note I4's inflation factors can push a variance above `max_variance`; I4 must test that
   overflow path rather than assume the ingress bound.

1. MINOR - Calibration records for lower incarnations accumulate on the persistent per-principal
   profile (I4 concern); I4 design must say they expire/are excised on incarnation advance, since
   rev 3 removed "excise the previous incarnation profile".

## Assessment

I4/I5 needs are otherwise met: epoch/calibration key fields are complete, verdict immutability with
revisions list, empty feature list and schema 0, priors declared, UNTESTED = unscored. Findings 1
and 2 are regressions introduced or exposed by rev 3 (the window ordering and the stream-scoped time
rule) and both cause valid observations to be refused terminally. Each is a small, local fix.

Tally: BLOCKING 0, MAJOR 2, MINOR 5.

I3-1a may proceed (policy/checker untouched by these findings). I3-1b/1c should wait for the two
fixes to be folded in and a quick re-check.

VERDICT: FAIL
